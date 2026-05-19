# ppc

The `ppc` profile models a 32-bit big-endian PowerPC kernel target.

Implementation: `src/arch/ppc/ops.cpp`.

Traits:

- Page size: 4096 bytes.
- General registers: 32.
- Interrupt model: MSR EE plus decrementer.
- Syscall model: `sc`.
- User transition: `rfi`.

Toolchain:

```sh
xmake toolchains -a ppc
xmake f -c -m debug -a ppc
```
