# arm32

The `arm32` profile models 32-bit ARM with little-endian memory and user/kernel
privilege separation.

Implementation: `src/arch/arm32/ops.cpp`.

The profile provides vector-table/GIC metadata, `svc` syscall metadata,
exception-return user-mode metadata, and guarded ARM inline assembly for cycle
counter reads, barriers, interrupt masking, and wait/yield behavior.

## Bring-up Plan

- Interrupts: vector table plus GIC or platform IRQ controller.
- Memory: short-descriptor page tables initially, LPAE later if needed.
- User mode: CPSR mode switch and exception return into user mode.
- Syscalls: `svc #0`.
- QEMU: `qemu-arm` for user-mode debug tests, `qemu-system-arm` for board-level tests.
