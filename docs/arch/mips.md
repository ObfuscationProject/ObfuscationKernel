# mips

The `mips` profile models a 32-bit big-endian MIPS kernel target.

Implementation:

- `src/arch/mips/ops.cpp`: architecture operations.
- `src/arch/mips/boot.S`: QEMU direct-entry bootstrap stack and `.bss`
  clearing.
- `src/arch/mips/platform.cpp`: QEMU Malta ISA COM1 serial through the PCI I/O
  window.
- `src/arch/mips/linker.ld`: ELF layout for the Malta kernel load address.

Traits:

- Page size: 4096 bytes.
- General registers: 32.
- Interrupt model: CP0 status/intctl.
- Syscall model: `syscall`.
- User transition: `eret`.

Toolchain:

```sh
xmake toolchains -a mips
xmake f -c -m debug -a mips
xmake qemu-test
```
