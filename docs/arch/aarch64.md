# aarch64

The `aarch64` profile models AArch64 EL1 kernel mode and EL0 user mode with 4
KiB pages.

## Bring-up Plan

- Interrupts: exception vector table and GICv2/GICv3 adapter.
- Memory: TTBR0 for user space, TTBR1 for kernel space, MAIR/TCR setup.
- User mode: `eret` from EL1 to EL0.
- Syscalls: `svc #0` with arguments in `x0` through `x5`.
- QEMU: `qemu-aarch64` for user-mode smoke, `qemu-system-aarch64` for EL tests.

