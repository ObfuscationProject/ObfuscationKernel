# ppc64

The `ppc64` profile models a 64-bit big-endian PowerPC kernel target.

Implementation: `src/arch/ppc64/ops.cpp`.

Traits:

- Page size: 4096 bytes.
- General registers: 32.
- Interrupt model: MSR EE plus decrementer.
- Syscall model: `sc`.
- User transition: `rfid`.

Toolchain:

```sh
xmake toolchains -a ppc64
xmake f -c -m debug -a ppc64
```
