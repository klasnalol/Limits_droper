#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAP_SIZE    (2 * 1024 * 1024)   // 2MB is enough for offsets like 0x59A0 safely

#include "mchbar_base.h"

static uint64_t rd64(volatile uint8_t *base, uint32_t off) {
    volatile uint32_t *p32 = (volatile uint32_t *)(base + off);
    uint64_t lo = p32[0];
    uint64_t hi = p32[1];
    return lo | (hi << 32);
}

int main(void) {
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("open(/dev/mem)"); return 1; }

    uint64_t mchbar_base = 0;
    char err[256] = {0};
    if (mchbar_get_base(&mchbar_base, err, sizeof(err)) != 0) {
        fprintf(stderr, "MCHBAR base discovery failed: %s\n", err[0] ? err : "unknown error");
        close(fd);
        return 1;
    }

    volatile uint8_t *mmio = mmap(NULL, MAP_SIZE, PROT_READ, MAP_SHARED, fd, mchbar_base);
    if (mmio == MAP_FAILED) { perror("mmap"); return 1; }

    struct { const char *name; uint32_t off; } regs[] = {
        {"PKG_POWER_LIMIT? (often)", 0x59A0},
        {"PKG_ENERGY_STATUS? (often)", 0x59B0},
        {"PKG_POWER_INFO? (often)", 0x59C0},
        {"PKG_PERF_STATUS? (often)", 0x59E0},
    };

    for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        uint64_t v = rd64(mmio, regs[i].off);
        printf("%-28s off=0x%04X val=0x%016" PRIx64 "\n", regs[i].name, regs[i].off, v);
    }

    munmap((void*)mmio, MAP_SIZE);
    close(fd);
    return 0;
}
