# loongarch64

The `loongarch64` profile is included as an additional 64-bit architecture
target.

Implementation: `src/arch/loongarch64/ops.cpp`.

The profile provides LoongArch exception/syscall/user-return metadata and
guarded LoongArch inline assembly for time reads, barriers, interrupt control,
and idle/yield behavior.

## Bring-up Plan

- Interrupts: LoongArch exception entry and interrupt controller adapter.
- Memory: LoongArch page-table format and privilege bits.
- User mode: architecture-specific exception return.
- Syscalls: architecture ABI syscall entry.
- QEMU: `qemu-loongarch64` for user-mode smoke when available.
