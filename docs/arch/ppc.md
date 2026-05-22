# ppc

The `ppc` profile models a 32-bit big-endian PowerPC kernel target.

Implementation:

- `src/arch/ppc/ops.cpp`: architecture operations.
- `src/arch/ppc/boot.S`: QEMU direct-entry bootstrap stack, `.bss` clearing, and
  e500 CCSR TLB setup.
- `src/arch/ppc/platform.cpp`: QEMU ppce500 NS16550 serial platform I/O.
- `src/arch/ppc/linker.ld`: ELF layout for the ppce500 kernel load address.

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
xmake qemu-test
```

QEMU uses `qemu-system-ppc -M ppce500 -kernel`. Early boot maps effective
`0xe0000000` to physical `0xfe0000000` so the CCSR serial window is reachable
before C++ platform output starts.
