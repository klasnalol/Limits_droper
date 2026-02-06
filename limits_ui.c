#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAP_SIZE    (2 * 1024 * 1024)
#define PL_OFF      0x59A0

#include "mchbar_base.h"

#define MSR_RAPL_POWER_UNIT  0x606
#define MSR_PKG_POWER_LIMIT  0x610

struct mmio_ctx {
    int fd;
    volatile uint8_t *base;
};

static uint64_t rd64(volatile uint8_t *base, uint32_t off) {
    volatile uint32_t *p32 = (volatile uint32_t *)(base + off);
    uint64_t lo = p32[0];
    uint64_t hi = p32[1];
    return lo | (hi << 32);
}

static void wr64(volatile uint8_t *base, uint32_t off, uint64_t v) {
    volatile uint32_t *p32 = (volatile uint32_t *)(base + off);
    p32[0] = (uint32_t)(v & 0xffffffffu);
    p32[1] = (uint32_t)(v >> 32);
    (void)p32[1];
}

static int write_text_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(text);
    ssize_t n = write(fd, text, len);
    int saved_errno = errno;
    close(fd);
    if (n != (ssize_t)len) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int write_powercap_uw(uint64_t pl1_uw, uint64_t pl2_uw) {
    char buf[32];
    const char *pl1_path = "/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw";
    const char *pl2_path = "/sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw";

    snprintf(buf, sizeof(buf), "%" PRIu64, pl1_uw);
    if (write_text_file(pl1_path, buf) != 0) {
        return -1;
    }
    snprintf(buf, sizeof(buf), "%" PRIu64, pl2_uw);
    if (write_text_file(pl2_path, buf) != 0) {
        return -1;
    }
    return 0;
}

static int open_mmio(struct mmio_ctx *ctx) {
    uint64_t mchbar_base = 0;
    char err[256] = {0};
    if (mchbar_get_base(&mchbar_base, err, sizeof(err)) != 0) {
        fprintf(stderr, "MCHBAR base discovery failed: %s\n", err[0] ? err : "unknown error");
        return -1;
    }

    ctx->fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (ctx->fd < 0) {
        perror("open(/dev/mem)");
        return -1;
    }

    ctx->base = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, mchbar_base);
    if (ctx->base == MAP_FAILED) {
        perror("mmap");
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }
    return 0;
}

static void close_mmio(struct mmio_ctx *ctx) {
    if (ctx->base && ctx->base != MAP_FAILED) {
        munmap((void *)ctx->base, MAP_SIZE);
        ctx->base = NULL;
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

static int open_msr(int cpu) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    return open(path, O_RDWR);
}

static int rdmsr(int fd, uint32_t reg, uint64_t *out) {
    ssize_t n = pread(fd, out, sizeof(*out), reg);
    if (n != (ssize_t)sizeof(*out)) {
        return -1;
    }
    return 0;
}

static int wrmsr(int fd, uint32_t reg, uint64_t val) {
    ssize_t n = pwrite(fd, &val, sizeof(val), reg);
    if (n != (ssize_t)sizeof(val)) {
        return -1;
    }
    return 0;
}

static uint64_t set_pl_units(uint64_t cur, uint16_t pl1_units, uint16_t pl2_units) {
    uint32_t lo = (uint32_t)(cur & 0xffffffffu);
    uint32_t hi = (uint32_t)(cur >> 32);

    lo = (lo & ~0x7FFFu) | ((uint32_t)pl1_units & 0x7FFFu);
    hi = (hi & ~0x7FFFu) | ((uint32_t)pl2_units & 0x7FFFu);

    return ((uint64_t)hi << 32) | lo;
}

static int read_line(char *buf, size_t sz) {
    if (!fgets(buf, (int)sz, stdin)) {
        return 0;
    }
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
    return 1;
}

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (s == end) {
        return 0;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int parse_double(const char *s, double *out) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (s == end) {
        return 0;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

static int prompt_int(const char *label, int *out) {
    char buf[128];
    printf("%s", label);
    fflush(stdout);
    if (!read_line(buf, sizeof(buf))) {
        return 0;
    }
    if (buf[0] == '\0') {
        return 0;
    }
    return parse_int(buf, out);
}

static int prompt_double(const char *label, double *out) {
    char buf[128];
    printf("%s", label);
    fflush(stdout);
    if (!read_line(buf, sizeof(buf))) {
        return 0;
    }
    if (buf[0] == '\0') {
        return 0;
    }
    if (buf[0] == 'q' || buf[0] == 'Q') {
        return 0;
    }
    return parse_double(buf, out);
}

static int confirm(const char *label) {
    char buf[16];
    printf("%s [y/N]: ", label);
    fflush(stdout);
    if (!read_line(buf, sizeof(buf))) {
        return 0;
    }
    return (buf[0] == 'y' || buf[0] == 'Y');
}

static void print_pl(const char *label, uint64_t val, double unit_watts) {
    uint16_t pl1 = (uint16_t)(val & 0x7FFFu);
    uint16_t pl2 = (uint16_t)((val >> 32) & 0x7FFFu);
    double pl1_w = (double)pl1 * unit_watts;
    double pl2_w = (double)pl2 * unit_watts;

    printf("%s\n", label);
    printf("  raw = 0x%016" PRIx64 "\n", val);
    printf("  PL1 = %u (%.2f W)\n", pl1, pl1_w);
    printf("  PL2 = %u (%.2f W)\n", pl2, pl2_w);
}

static void show_status(int msr_fd, struct mmio_ctx *mmio, double unit_watts) {
    uint64_t msr = 0;
    uint64_t mmio_val = rd64(mmio->base, PL_OFF);

    if (rdmsr(msr_fd, MSR_PKG_POWER_LIMIT, &msr) != 0) {
        fprintf(stderr, "read MSR 0x%X failed: %s\n", MSR_PKG_POWER_LIMIT, strerror(errno));
        return;
    }

    print_pl("MSR  IA32_PKG_POWER_LIMIT (0x610)", msr, unit_watts);
    print_pl("MMIO MCHBAR PL (0x59A0)", mmio_val, unit_watts);
}

static int set_limits(int msr_fd, struct mmio_ctx *mmio, double unit_watts) {
    double pl1_w = 0.0;
    double pl2_w = 0.0;
    int target = 0;

    if (!prompt_double("PL1 watts (q to cancel): ", &pl1_w)) {
        printf("Canceled.\n");
        return 0;
    }
    if (!prompt_double("PL2 watts (q to cancel): ", &pl2_w)) {
        printf("Canceled.\n");
        return 0;
    }

    if (pl1_w <= 0.0 || pl2_w <= 0.0 || pl1_w > 5000.0 || pl2_w > 5000.0) {
        fprintf(stderr, "Refusing unusual values.\n");
        return -1;
    }

    printf("Target: 1) MSR  2) MMIO  3) Both\n");
    if (!prompt_int("Select target: ", &target)) {
        printf("Canceled.\n");
        return 0;
    }
    if (target < 1 || target > 3) {
        fprintf(stderr, "Invalid target.\n");
        return -1;
    }

    uint16_t pl1_units = (uint16_t)llround(pl1_w / unit_watts);
    uint16_t pl2_units = (uint16_t)llround(pl2_w / unit_watts);

    if (pl1_units == 0 || pl2_units == 0 || pl1_units > 0x7FFFu || pl2_units > 0x7FFFu) {
        fprintf(stderr, "Converted units out of range.\n");
        return -1;
    }

    if (target == 1 || target == 3) {
        uint64_t cur = 0;
        if (rdmsr(msr_fd, MSR_PKG_POWER_LIMIT, &cur) != 0) {
            fprintf(stderr, "read MSR 0x%X failed: %s\n", MSR_PKG_POWER_LIMIT, strerror(errno));
            return -1;
        }
        uint64_t next = set_pl_units(cur, pl1_units, pl2_units);
        printf("MSR  new = 0x%016" PRIx64 "\n", next);
        if (confirm("Write MSR?")) {
            if (wrmsr(msr_fd, MSR_PKG_POWER_LIMIT, next) != 0) {
                fprintf(stderr, "write MSR 0x%X failed: %s\n", MSR_PKG_POWER_LIMIT, strerror(errno));
                return -1;
            }
        }
    }

    if (target == 2 || target == 3) {
        uint64_t cur = rd64(mmio->base, PL_OFF);
        uint64_t next = set_pl_units(cur, pl1_units, pl2_units);
        printf("MMIO new = 0x%016" PRIx64 "\n", next);
        if (confirm("Write MMIO?")) {
            wr64(mmio->base, PL_OFF, next);
        }
    }

    if (confirm("Write kernel powercap (intel-rapl)?")) {
        uint64_t pl1_uw = (uint64_t)llround(pl1_w * 1000000.0);
        uint64_t pl2_uw = (uint64_t)llround(pl2_w * 1000000.0);
        if (pl1_uw == 0 || pl2_uw == 0) {
            fprintf(stderr, "Invalid powercap values.\n");
            return -1;
        }
        if (write_powercap_uw(pl1_uw, pl2_uw) != 0) {
            fprintf(stderr, "write powercap failed: %s\n", strerror(errno));
            return -1;
        }
        printf("Wrote powercap PL1=%" PRIu64 "uW PL2=%" PRIu64 "uW\n", pl1_uw, pl2_uw);
    }

    return 0;
}

static int sync_limits(int msr_fd, struct mmio_ctx *mmio) {
    int dir = 0;

    printf("Sync: 1) MSR -> MMIO  2) MMIO -> MSR\n");
    if (!prompt_int("Select direction: ", &dir)) {
        printf("Canceled.\n");
        return 0;
    }

    if (dir == 1) {
        uint64_t msr = 0;
        if (rdmsr(msr_fd, MSR_PKG_POWER_LIMIT, &msr) != 0) {
            fprintf(stderr, "read MSR 0x%X failed: %s\n", MSR_PKG_POWER_LIMIT, strerror(errno));
            return -1;
        }
        printf("MMIO <- 0x%016" PRIx64 "\n", msr);
        if (confirm("Write MMIO?")) {
            wr64(mmio->base, PL_OFF, msr);
        }
    } else if (dir == 2) {
        uint64_t mmio_val = rd64(mmio->base, PL_OFF);
        printf("MSR  <- 0x%016" PRIx64 "\n", mmio_val);
        if (confirm("Write MSR?")) {
            if (wrmsr(msr_fd, MSR_PKG_POWER_LIMIT, mmio_val) != 0) {
                fprintf(stderr, "write MSR 0x%X failed: %s\n", MSR_PKG_POWER_LIMIT, strerror(errno));
                return -1;
            }
        }
    } else {
        fprintf(stderr, "Invalid direction.\n");
        return -1;
    }

    return 0;
}

int main(void) {
    struct mmio_ctx mmio = { .fd = -1, .base = NULL };
    int msr_fd = -1;
    uint64_t rapl_units = 0;
    int power_unit = 0;
    double unit_watts = 0.0;

    if (open_mmio(&mmio) != 0) {
        return 1;
    }

    msr_fd = open_msr(0);
    if (msr_fd < 0) {
        fprintf(stderr, "open(/dev/cpu/0/msr) failed: %s\n", strerror(errno));
        close_mmio(&mmio);
        return 1;
    }

    if (rdmsr(msr_fd, MSR_RAPL_POWER_UNIT, &rapl_units) != 0) {
        fprintf(stderr, "read MSR 0x%X failed: %s\n", MSR_RAPL_POWER_UNIT, strerror(errno));
        close(msr_fd);
        close_mmio(&mmio);
        return 1;
    }

    power_unit = (int)(rapl_units & 0x0F);
    unit_watts = 1.0 / (double)(1u << power_unit);

    printf("Limits UI (MSR 0x610 + MCHBAR 0x59A0)\n");
    printf("Power unit: 2^-%d W = %.6f W\n\n", power_unit, unit_watts);

    for (;;) {
        int choice = 0;
        printf("Menu:\n");
        printf("  1) Show current limits\n");
        printf("  2) Set PL1/PL2 (watts)\n");
        printf("  3) Sync MSR and MMIO\n");
        printf("  4) Exit\n");

        if (!prompt_int("Select: ", &choice)) {
            printf("Exiting.\n");
            break;
        }

        if (choice == 1) {
            show_status(msr_fd, &mmio, unit_watts);
        } else if (choice == 2) {
            if (set_limits(msr_fd, &mmio, unit_watts) != 0) {
                fprintf(stderr, "Failed to set limits.\n");
            }
        } else if (choice == 3) {
            if (sync_limits(msr_fd, &mmio) != 0) {
                fprintf(stderr, "Failed to sync.\n");
            }
        } else if (choice == 4) {
            printf("Done.\n");
            break;
        } else {
            printf("Unknown choice.\n");
        }

        printf("\n");
    }

    close(msr_fd);
    close_mmio(&mmio);
    return 0;
}
