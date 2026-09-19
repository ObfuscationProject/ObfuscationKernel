# mips64

The `mips64` profile boots a 64-bit big-endian MIPS kernel and runs native n64
user instructions on QEMU Malta with `MIPS64R2-generic`.

Implementation:

- `src/arch/mips64/ops.cpp`: architecture operations.
- `src/arch/mips64/boot.S`: QEMU direct-entry bootstrap stack and `.bss`
  clearing.
- `src/arch/mips64/platform.cpp`: QEMU Malta ISA COM1 serial through the PCI I/O
  window. Malta does not expose fw_cfg/standalone `ramfb`, so the QEMU window
  path uses serial VC output.
- `src/arch/mips64/linker.ld`: 64-bit big-endian ELF layout for the Malta kernel
  load address.
- `src/arch/mips64/execution.cpp` and `execution.S`: KSU user execution through
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
xmake f -c -y -m debug -a mips64
xmake qemu-test
python3 scripts/test_native_mips.py --arch mips64
```

QEMU uses `qemu-system-mips64 -M malta -cpu MIPS64R2-generic -kernel` to match
the `-march=mips64r2 -mabi=64` freestanding build.

The hardware lacks NX. User/kernel and read-only isolation are verified; code
can still execute from a readable data page. See [native execution details and
limits](mips-native.md) for the syscall ABI, TLS emulation and hardware probes.
