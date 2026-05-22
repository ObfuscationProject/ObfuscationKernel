# mips64

The `mips64` profile models a 64-bit big-endian MIPS kernel target.

Implementation:

- `src/arch/mips64/ops.cpp`: architecture operations.
- `src/arch/mips64/boot.S`: QEMU direct-entry bootstrap stack and `.bss`
  clearing.
- `src/arch/mips64/platform.cpp`: QEMU Malta ISA COM1 serial through the PCI I/O
  window.
- `src/arch/mips64/linker.ld`: 64-bit big-endian ELF layout for the Malta kernel
  load address.

Traits:

- Page size: 4096 bytes.
- General registers: 32.
- Interrupt model: CP0 status/intctl.
- Syscall model: `syscall`.
- User transition: `eret`.

Toolchain:

```sh
xmake toolchains -a mips64
xmake f -c -m debug -a mips64
xmake qemu-test
```

QEMU uses `qemu-system-mips64 -M malta -cpu MIPS64R2-generic -kernel` to match
the `-march=mips64r2 -mabi=64` freestanding build.
