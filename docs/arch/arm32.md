# arm32

The `arm32` profile models 32-bit ARM with little-endian memory and user/kernel
privilege separation.

## Bring-up Plan

- Interrupts: vector table plus GIC or platform IRQ controller.
- Memory: short-descriptor page tables initially, LPAE later if needed.
- User mode: CPSR mode switch and exception return into user mode.
- Syscalls: `svc #0`.
- QEMU: `qemu-arm` for user-mode smoke, `qemu-system-arm` for board-level tests.

