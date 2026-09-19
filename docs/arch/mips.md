# mips

The `mips` profile boots a 32-bit big-endian MIPS kernel and runs native O32
user instructions on the default QEMU Malta `24Kf` CPU.

Implementation:

- `src/arch/mips/ops.cpp`: architecture operations.
- `src/arch/mips/boot.S`: QEMU direct-entry bootstrap stack and `.bss`
  clearing.
- `src/arch/mips/platform.cpp`: QEMU Malta ISA COM1 serial through the PCI I/O
  window. Malta does not expose fw_cfg/standalone `ramfb`, so the QEMU window
  path uses serial VC output.
- `src/arch/mips/linker.ld`: ELF layout for the Malta kernel load address.
- `src/arch/mips/execution.cpp` and `execution.S`: KSU user execution through
  the shared MIPS backend, including software TLB refill, CP0 timer preemption,
  syscall/fault return and complete ABI register state.

Traits:

- Page size: 4096 bytes.
- General registers: 32.
- Interrupt model: CP0 status/intctl.
- Syscall model: `syscall`.
- User transition: `eret`.

Toolchain:

```sh
xmake f -c -y -m debug -a mips
xmake qemu-test
python3 scripts/test_native_mips.py --arch mips
```

The hardware lacks NX. User/kernel and read-only isolation are verified; code
can still execute from a readable data page. See [native execution details and
limits](mips-native.md) for the syscall ABI, TLS emulation and hardware probes.
