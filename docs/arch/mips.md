# mips

The `mips` profile models a 32-bit big-endian MIPS kernel target.

Implementation: `src/arch/mips/ops.cpp`.

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
```
