# loongarch64

The `loongarch64` profile is included as an additional 64-bit architecture
target. It currently uses the generic simulated architecture operations.

## Bring-up Plan

- Interrupts: LoongArch exception entry and interrupt controller adapter.
- Memory: LoongArch page-table format and privilege bits.
- User mode: architecture-specific exception return.
- Syscalls: architecture ABI syscall entry.
- QEMU: `qemu-loongarch64` for user-mode smoke when available.

