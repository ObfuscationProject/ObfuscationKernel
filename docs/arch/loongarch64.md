# loongarch64

The `loongarch64` profile is included as an additional 64-bit architecture
target.

Implementation:

- `src/arch/loongarch64/ops.cpp`: architecture operations.
- `src/arch/loongarch64/boot.S`: QEMU direct-entry bootstrap stack and `.bss`
  clearing.
- `src/arch/loongarch64/platform.cpp`: QEMU virt NS16550 serial and ramfb
  platform I/O.
- `src/arch/loongarch64/linker.ld`: ELF layout at the QEMU virt high RAM base.

The profile provides LoongArch exception/syscall/user-return metadata and
guarded LoongArch inline assembly for time reads, barriers, interrupt control,
and idle/yield behavior.

## QEMU

`qemu-system-loongarch64 -M virt -kernel` boots the linked ELF at `0x90000000`.
The freestanding build disables LSX/LASX auto-vectorization with `-msimd=none`
so early boot does not depend on vector unit enablement.
