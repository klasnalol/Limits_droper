#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mchbar_base.h"

#define MAP_SIZE    (2 * 1024 * 1024)
#define MSR_RAPL_POWER_UNIT 0x606

static uint64_t rd64(volatile uint8_t *base, uint32_t off) {
    volatile uint32_t *p32 = (volatile uint32_t *)(base + off);
    uint64_t lo = p32[0];
    uint64_t hi = p32[1];
    return lo | (hi << 32);
}

static int open_msr(int cpu) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    return open(path, O_RDONLY);
}

static int rdmsr(int fd, uint32_t reg, uint64_t *out) {
    if (lseek(fd, (off_t)reg, SEEK_SET) < 0) {
        return -1;
    }
    ssize_t n = read(fd, out, sizeof(*out));
    if (n != (ssize_t)sizeof(*out)) {
        return -1;
    }
    return 0;
}

static int parse_u16(const char *s, uint16_t *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (s == end) {
        return 0;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    if (v > 0xFFFFu) {
        return 0;
    }
    *out = (uint16_t)v;
    return 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [--pl1 WATTS] [--pl2 WATTS]\n"
        "  %s --units PL1_UNITS PL2_UNITS\n"
        "  %s --any [--pl1 WATTS] [--pl2 WATTS]\n"
        "\n"
        "Defaults: PL1=55W PL2=157W (converted using MSR_RAPL_POWER_UNIT)\n"
        "Notes:\n"
        "  --any ignores enable bits (bit 15) when matching.\n",
        argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    double pl1_w = 55.0;
    double pl2_w = 157.0;
    int use_units = 0;
    uint16_t pl1_units = 0;
    uint16_t pl2_units = 0;
    int require_enable = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "--any") == 0) {
            require_enable = 0;
            continue;
        }
        if (strcmp(argv[i], "--units") == 0) {
            if (i + 2 >= argc) {
                fprintf(stderr, "Need PL1_UNITS PL2_UNITS after --units.\n");
                return 2;
            }
            if (!parse_u16(argv[i + 1], &pl1_units) || !parse_u16(argv[i + 2], &pl2_units)) {
                fprintf(stderr, "Invalid units (use decimal or 0x hex).\n");
                return 2;
            }
            use_units = 1;
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "--pl1") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Need watts after --pl1.\n");
                return 2;
            }
            pl1_w = atof(argv[i + 1]);
            i++;
            continue;
        }
        if (strcmp(argv[i], "--pl2") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Need watts after --pl2.\n");
                return 2;
            }
            pl2_w = atof(argv[i + 1]);
            i++;
            continue;
        }
        fprintf(stderr, "Unknown arg: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }

    double unit_watts = 0.0;
    if (!use_units) {
        int msr_fd = open_msr(0);
        if (msr_fd < 0) {
            fprintf(stderr, "open(/dev/cpu/0/msr) failed: %s\n", strerror(errno));
            return 1;
        }
        uint64_t rapl_units = 0;
        if (rdmsr(msr_fd, MSR_RAPL_POWER_UNIT, &rapl_units) != 0) {
            fprintf(stderr, "read MSR 0x%X failed: %s\n", MSR_RAPL_POWER_UNIT, strerror(errno));
            close(msr_fd);
            return 1;
        }
        close(msr_fd);

        int power_unit = (int)(rapl_units & 0x0F);
        unit_watts = 1.0 / (double)(1u << power_unit);
        pl1_units = (uint16_t)llround(pl1_w / unit_watts);
        pl2_units = (uint16_t)llround(pl2_w / unit_watts);
    }

    if (pl1_units == 0 || pl2_units == 0 || pl1_units > 0x7FFFu || pl2_units > 0x7FFFu) {
        fprintf(stderr, "Computed units out of range. PL1=0x%X PL2=0x%X\n", pl1_units, pl2_units);
        return 1;
    }

    uint64_t mchbar_base = 0;
    char err[256] = {0};
    if (mchbar_get_base(&mchbar_base, err, sizeof(err)) != 0) {
        fprintf(stderr, "MCHBAR base discovery failed: %s\n", err[0] ? err : "unknown error");
        return 1;
    }

    int mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "open(/dev/mem) failed: %s\n", strerror(errno));
        return 1;
    }

    volatile uint8_t *mmio = mmap(NULL, MAP_SIZE, PROT_READ, MAP_SHARED, mem_fd, mchbar_base);
    if (mmio == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(mem_fd);
        return 1;
    }

    if (use_units) {
        printf("Scanning MCHBAR @ 0x%016" PRIx64 " for units PL1=0x%X PL2=0x%X (require_enable=%d)\n",
               mchbar_base, pl1_units, pl2_units, require_enable);
    } else {
        printf("Scanning MCHBAR @ 0x%016" PRIx64 " for PL1=%.3fW PL2=%.3fW (units 0x%X/0x%X, unit=%.6fW, require_enable=%d)\n",
               mchbar_base, pl1_w, pl2_w, pl1_units, pl2_units, unit_watts, require_enable);
    }

    int found = 0;
    for (uint32_t off = 0; off + 8 <= MAP_SIZE; off += 8) {
        uint64_t v = rd64(mmio, off);
        uint32_t lo = (uint32_t)(v & 0xffffffffu);
        uint32_t hi = (uint32_t)(v >> 32);
        if ((lo & 0x7FFFu) != pl1_units) {
            continue;
        }
        if ((hi & 0x7FFFu) != pl2_units) {
            continue;
        }
        if (require_enable) {
            if ((lo & 0x8000u) == 0 || (hi & 0x8000u) == 0) {
                continue;
            }
        }
        printf("match off=0x%05X val=0x%016" PRIx64 " lo=0x%08X hi=0x%08X\n", off, v, lo, hi);
        found++;
    }

    if (found == 0) {
        printf("No matches found.\n");
    }

    munmap((void *)mmio, MAP_SIZE);
    close(mem_fd);
    return 0;
}
