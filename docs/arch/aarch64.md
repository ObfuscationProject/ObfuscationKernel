# aarch64

The `aarch64` profile has a native EL1/EL0 execution backend for QEMU virt
with GICv2, in addition to the architecture-neutral model tests.

## Boot and platform

- `boot.S` supplies the Linux `Image` header, accepts EL1 or EL2 boot entry,
  enters EL1, establishes the kernel stack and clears `.bss`.
- `platform.cpp` supplies PL011 serial I/O, RAM framebuffer and virtio input.
- `linker.ld` places the kernel at `0x40080000`.
- `ops.cpp` provides statically selected architecture instructions.

## Native execution

`execution.cpp` and `execution.S` implement `ok::arch::execution`:

- 4 KiB user pages, 39-bit virtual addressing, TTBR0 address-space switching,
  explicit MAIR/TCR setup and TLB invalidation.
- User window `[0x10000000, 0x40000000)`, distinct from kernel RAM and device
  MMIO. Kernel identity mappings remain supervisor-only. User pages enforce
  read-only and execute-never permissions; writable executable mappings fail.
- Real EL0 entry using `eret` and synchronous return to the original kernel
  stack on `svc`, architectural physical timer IRQ 30, or CPU fault.
- SVC number in `x8`, arguments in `x0` through `x5`, return value in `x0`.
- Saved `x0` through `x30`, SP, PC, NZCV, all 32 SIMD registers, FPCR/FPSR,
  TPIDR_EL0 and TPIDRRO_EL0. The shared frame's register slot 31 stores FPCR
  in its low 32 bits and FPSR in its high 32 bits; `fs_base`/`gs_base` store
  the two thread-ID registers.
- Fault classification from ESR_EL1/FAR_EL1, safe user-copy validation across
  every requested page, and stable generation-checked address-space handles.

The current backend supports one executing CPU. Each address space owns a
level-1 root, a level-2 directory and one level-3 table per populated 2 MiB,
all allocated from the runtime's table storage and released with the space;
the slot table grows with the number of spaces. Application backing pages
belong to the caller; the backend owns page tables only.
The initial identity layout assumes QEMU virt RAM in `[0x40000000, 0x80000000)`
and MMIO below `0x10000000`; it does not discover memory from a device tree.
GICv3, SVE, SMP execution and general physical-page allocation remain future
work. This is a native mechanism, not a claim of complete POSIX conformance.

## Hardware acceptance test

From the kernel repository:

```sh
python3 scripts/test_native_aarch64.py --toolchain toolchains/aarch64-elf
```

The standalone test builds independently of xmake configuration and runs
QEMU twice, entering at EL1 and EL2. It executes actual EL0 machine code and
checks SVC registers, TLS, SIMD/FPCR preservation, distinct physical backing
at the same virtual address, timer preemption of a non-cooperative loop,
supervisor isolation, read-only/NX/unmapped faults, invalid instructions,
user-copy bounds and kernel TTBR0 restoration. Successful runs emit
`OK_NATIVE_TEST_PASS scope=hardware architecture=aarch64`; serial logs are
written under `build/native-tests/aarch64`.

`xmake qemu-test` separately validates the standalone debug kernel, while the
OS integration tests validate applications linked against the exported SDK.
Metadata model tests do not substitute for either execution path.
