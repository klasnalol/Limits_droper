#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define MAP_SIZE    (2 * 1024 * 1024)
#define PL_OFF      0x59A0

#include "mchbar_base.h"

static uint64_t rd64(volatile uint8_t *base, uint32_t off) {
    volatile uint32_t *p32 = (volatile uint32_t *)(base + off);
    uint64_t lo = p32[0];
    uint64_t hi = p32[1];
    return lo | (hi << 32);
}

static void wr64(volatile uint8_t *base, uint32_t off, uint64_t v) {
    volatile uint32_t *p32 = (volatile uint32_t *)(base + off);
    // write low then high (common practice)
    p32[0] = (uint32_t)(v & 0xffffffffu);
    p32[1] = (uint32_t)(v >> 32);
    // small barrier-ish readback
    (void)p32[1];
}

static uint32_t replace_power_field(uint32_t cur, uint16_t new_units) {
    // Replace bits 14:0 only (power field). Preserve enable/clamp/time window bits.
    uint32_t out = (cur & ~0x7FFFu) | ((uint32_t)new_units & 0x7FFFu);
    return out;
}

int main(int argc, char **argv) {
    if (argc < 2 || (!strcmp(argv[1], "--help"))) {
        fprintf(stderr,
            "Usage:\n"
            "  %s --set PL1_W PL2_W\n"
            "  %s --restore HEX64\n"
            "\nExamples:\n"
            "  sudo %s --set 150 170\n"
            "  sudo %s --restore 0x004284e800df81b8\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open(/dev/mem)"); return 1; }

    uint64_t mchbar_base = 0;
    char err[256] = {0};
    if (mchbar_get_base(&mchbar_base, err, sizeof(err)) != 0) {
        fprintf(stderr, "MCHBAR base discovery failed: %s\n", err[0] ? err : "unknown error");
        close(fd);
        return 1;
    }

    volatile uint8_t *mmio = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mchbar_base);
    if (mmio == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    uint64_t orig = rd64(mmio, PL_OFF);
    printf("ORIG  [0x%04X] = 0x%016" PRIx64 "\n", PL_OFF, orig);

    uint64_t target = orig;

    if (!strcmp(argv[1], "--restore")) {
        if (argc < 3) { fprintf(stderr, "Need HEX64.\n"); return 2; }
        uint64_t v = 0;
        if (sscanf(argv[2], "%" SCNx64, &v) != 1) {
            // allow 0x prefix
            if (sscanf(argv[2], "0x%" SCNx64, &v) != 1) {
                fprintf(stderr, "Bad HEX64.\n");
                return 2;
            }
        }
        target = v;
        printf("RESTORE target = 0x%016" PRIx64 "\n", target);
    } else if (!strcmp(argv[1], "--set")) {
        if (argc < 4) { fprintf(stderr, "Need PL1_W PL2_W.\n"); return 2; }
        int pl1w = atoi(argv[2]);
        int pl2w = atoi(argv[3]);
        if (pl1w <= 0 || pl2w <= 0 || pl1w > 500 || pl2w > 500) {
            fprintf(stderr, "Refusing weird values.\n");
            return 2;
        }
        // assume 1/8 W units
        uint16_t pl1_units = (uint16_t)(pl1w * 8);
        uint16_t pl2_units = (uint16_t)(pl2w * 8);

        uint32_t lo = (uint32_t)(orig & 0xffffffffu);
        uint32_t hi = (uint32_t)(orig >> 32);

        uint32_t lo2 = replace_power_field(lo, pl1_units);
        uint32_t hi2 = replace_power_field(hi, pl2_units);

        target = ((uint64_t)hi2 << 32) | lo2;

        printf("SET  PL1=%dW (0x%X)  PL2=%dW (0x%X)\n", pl1w, pl1_units, pl2w, pl2_units);
        printf("NEW  lo32=0x%08x hi32=0x%08x\n", lo2, hi2);
    } else {
        fprintf(stderr, "Unknown mode.\n");
        return 2;
    }

    wr64(mmio, PL_OFF, target);
    uint64_t after = rd64(mmio, PL_OFF);

    printf("AFTER [0x%04X] = 0x%016" PRIx64 "\n", PL_OFF, after);
    printf("Restore command:\n  sudo %s --restore 0x%016" PRIx64 "\n", argv[0], orig);

    munmap((void*)mmio, MAP_SIZE);
    close(fd);
    return 0;
}
