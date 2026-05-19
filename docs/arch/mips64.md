# mips64

The `mips64` profile models a 64-bit big-endian MIPS kernel target.

Implementation: `src/arch/mips64/ops.cpp`.

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
```
