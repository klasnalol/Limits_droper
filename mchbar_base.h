#ifndef LIMITS_DROPER_MCHBAR_BASE_H
#define LIMITS_DROPER_MCHBAR_BASE_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int mchbar_read_sysfs_hex_u32(const char *path, uint32_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    unsigned long val = 0;
    if (sscanf(buf, "0x%lx", &val) != 1) {
        return -1;
    }
    *out = (uint32_t)val;
    return 0;
}

static int mchbar_is_intel_host_bridge(const char *dev_path) {
    char class_path[PATH_MAX];
    char vendor_path[PATH_MAX];
    snprintf(class_path, sizeof(class_path), "%s/class", dev_path);
    snprintf(vendor_path, sizeof(vendor_path), "%s/vendor", dev_path);

    uint32_t class_val = 0;
    uint32_t vendor_val = 0;
    if (mchbar_read_sysfs_hex_u32(class_path, &class_val) != 0) {
        return 0;
    }
    if (mchbar_read_sysfs_hex_u32(vendor_path, &vendor_val) != 0) {
        return 0;
    }

    if (vendor_val != 0x8086u) {
        return 0;
    }
    if ((class_val & 0xFFFF00u) != 0x060000u) {
        return 0;
    }
    return 1;
}

static int mchbar_find_host_bridge_config(char *out_path, size_t out_sz, char *err, size_t err_sz) {
    const char *primary = "/sys/bus/pci/devices/0000:00:00.0";
    if (mchbar_is_intel_host_bridge(primary)) {
        size_t len = strlen(primary);
        const char *suffix = "/config";
        size_t suffix_len = strlen(suffix);
        if (len + suffix_len + 1 > out_sz) {
            if (err && err_sz) {
                snprintf(err, err_sz, "PCI config path too long");
            }
            return -1;
        }
        memcpy(out_path, primary, len);
        memcpy(out_path + len, suffix, suffix_len + 1);
        return 0;
    }

    DIR *dir = opendir("/sys/bus/pci/devices");
    if (!dir) {
        if (err && err_sz) {
            snprintf(err, err_sz, "open /sys/bus/pci/devices failed: %s", strerror(errno));
        }
        return -1;
    }

    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char dev_path[PATH_MAX];
        snprintf(dev_path, sizeof(dev_path), "/sys/bus/pci/devices/%s", ent->d_name);
        if (!mchbar_is_intel_host_bridge(dev_path)) {
            continue;
        }
        size_t len = strlen(dev_path);
        const char *suffix = "/config";
        size_t suffix_len = strlen(suffix);
        if (len + suffix_len + 1 > out_sz) {
            closedir(dir);
            if (err && err_sz) {
                snprintf(err, err_sz, "PCI config path too long");
            }
            return -1;
        }
        memcpy(out_path, dev_path, len);
        memcpy(out_path + len, suffix, suffix_len + 1);
        closedir(dir);
        return 0;
    }

    closedir(dir);
    if (err && err_sz) {
        snprintf(err, err_sz, "Intel host bridge not found in /sys/bus/pci/devices");
    }
    return -1;
}

static int mchbar_read_config_u64(const char *config_path, off_t offset, uint64_t *out, char *err, size_t err_sz) {
    int fd = open(config_path, O_RDONLY);
    if (fd < 0) {
        if (err && err_sz) {
            snprintf(err, err_sz, "open PCI config failed: %s", strerror(errno));
        }
        return -1;
    }

    if (lseek(fd, offset, SEEK_SET) < 0) {
        if (err && err_sz) {
            snprintf(err, err_sz, "seek PCI config +0x%lx failed: %s", (unsigned long)offset, strerror(errno));
        }
        close(fd);
        return -1;
    }

    uint64_t val = 0;
    ssize_t n = read(fd, &val, sizeof(val));
    int saved_errno = errno;
    close(fd);
    if (n != (ssize_t)sizeof(val)) {
        if (err && err_sz) {
            if (n < 0) {
                snprintf(err, err_sz, "read PCI config +0x%lx failed: %s", (unsigned long)offset, strerror(saved_errno));
            } else {
                snprintf(err, err_sz, "short read PCI config +0x%lx (%zd bytes)", (unsigned long)offset, n);
            }
        }
        return -1;
    }

    *out = val;
    return 0;
}

static int mchbar_get_base(uint64_t *out_base, char *err, size_t err_sz) {
    char config_path[PATH_MAX];
    if (mchbar_find_host_bridge_config(config_path, sizeof(config_path), err, err_sz) != 0) {
        return -1;
    }

    uint64_t val = 0;
    if (mchbar_read_config_u64(config_path, 0x48, &val, err, err_sz) != 0) {
        return -1;
    }

    if ((val & 0x1u) == 0) {
        if (err && err_sz) {
            snprintf(err, err_sz, "MCHBAR appears disabled (config 0x48 = 0x%016llx)", (unsigned long long)val);
        }
        return -1;
    }

    uint64_t base = val & ~0xFFFULL;
    if (base == 0) {
        if (err && err_sz) {
            snprintf(err, err_sz, "MCHBAR base resolved to 0 (config 0x48 = 0x%016llx)", (unsigned long long)val);
        }
        return -1;
    }

    *out_base = base;
    return 0;
}

#endif
