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

The local `ppc64` wrapper uses GCC's supported big-endian
`powerpc64-linux-gnu` target internally. The xmake profile remains freestanding;
this only changes the compiler target triple used to build the cross toolchain.

```sh
xmake toolchains -a ppc64
xmake f -c -m debug -a ppc64
```
