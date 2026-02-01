# Limits_droper

Small utilities for reading/writing Intel package power limits via MCHBAR MMIO and MSRs.

## Purpose

This project was made to bypass enforced power limits on a specific test setup by directly adjusting package power limit registers over MSR and MCHBAR MMIO. It has only been tested on an ES i7-13700HX (Q1K3) on an ASUS PRIME B660M-K D4, to bypass 55W (PL1) and 157W (PL2) limits. Other CPUs, steppings, or boards may behave differently.

## What’s here

- `mchbar_read.c`: read a few MCHBAR registers (including the package power limit window at 0x59A0).
- `mchbar_pl_write.c`: write the MCHBAR package power limit register (0x59A0).
- `limits_ui.c`: interactive CLI UI to view/set PL1/PL2 in watts and sync MSR <-> MMIO.

## Requirements

- Linux with access to `/dev/mem` (root).
- MSR driver for `/dev/cpu/0/msr` (load with `modprobe msr`).
- Root privileges for reads/writes.

## Build

```bash
gcc -std=c11 -Wall -Wextra -O2 -o mchbar_read mchbar_read.c
gcc -std=c11 -Wall -Wextra -O2 -o mchbar_pl_write mchbar_pl_write.c
gcc -std=c11 -Wall -Wextra -O2 -o limits_ui limits_ui.c -lm
```

## Usage

Read MCHBAR values:
```bash
sudo ./mchbar_read
```

Write MCHBAR package limits (PL1/PL2):
```bash
sudo ./mchbar_pl_write --set 150 170
sudo ./mchbar_pl_write --restore 0x004284e800df81b8
```

Interactive UI (read/set/sync MSR + MMIO):
```bash
sudo ./limits_ui
```

## Notes

- MCHBAR base is assumed to be `0xFEDC0000`, package power limit register at offset `0x59A0`.
- MSR power unit is taken from `IA32_RAPL_POWER_UNIT` (0x606) and applied when converting watts.
- Power limits are written to `IA32_PKG_POWER_LIMIT` (0x610) and/or MCHBAR 0x59A0.
- Tested only on ES i7-13700HX (Q1K3) on PRIME B660M-K D4, used to bypass 55W and 157W limits. Other CPUs/boards may differ.

## Safety

Writing MSRs/MMIO can destabilize a system or damage hardware. Use at your own risk.
