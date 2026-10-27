# ppc

The `ppc` profile models a 32-bit big-endian PowerPC kernel target.

Implementation:

- `src/arch/ppc/ops.cpp`: architecture operations.
- `src/arch/ppc/boot.S`: QEMU direct-entry bootstrap stack, `.bss` clearing, and
  e500 CCSR TLB setup.
- `src/arch/ppc/platform.cpp`: QEMU ppce500 NS16550 serial platform I/O.
  ppce500 does not expose fw_cfg/standalone `ramfb`, so the QEMU window path
  uses serial VC output.
- `src/arch/ppc/linker.ld`: ELF layout for the ppce500 kernel load address.

Traits:

- Page size: 4096 bytes.
- General registers: 32.
- Interrupt model: MSR EE plus decrementer.
- Syscall model: `sc`.
- User transition: `rfi`.

Toolchain:

```sh
xmake f -c -y -m debug -a ppc
xmake qemu-test
```

If a local cross-toolchain directory contains the compiler but no QEMU binary,
configure the explicit host executable with
`--qemu_binary=/usr/bin/qemu-system-ppc` alongside `--systoolchain_dir`.
The QEMU harness still validates the executable path and the PPC boot markers.

QEMU uses `qemu-system-ppc -M ppce500 -kernel`. Early boot maps effective
`0xe0000000` to physical `0xfe0000000` so the CCSR serial window is reachable
before C++ platform output starts. Boot also replaces QEMU's 64 MiB RAM TLB
entry with a 256 MiB supervisor mapping before clearing BSS or using the linked
boot stack. BookE MAS1 TSIZE 8 encodes 64 MiB; TSIZE 9 encodes 256 MiB.

## Native execution on e500v2

`src/arch/ppc/execution.cpp` and `execution.S` implement real BookE PR=1
execution on the existing ppce500 machine and its default e500v2 CPU. The
profile remains ELF32 big-endian (`EM_PPC`). Both kernel and user code use
`-msoft-float -msdata=none`: this CPU has SPE, not the classic PowerPC FPU,
and the installed GCC does not supply an SPE floating-point ABI.

- Kernel RAM `[0, 256 MiB)` is identity-mapped with supervisor permissions.
  Initialization removes the user permissions from QEMU's bootstrap RAM entry.
  The supervisor CCSR mapping remains in TLB1 slot 1. Slot 2 permanently maps
  `[0xc0000000, 0xd0000000)` as supervisor read/write, cache-inhibited and guarded
  PCI MMIO. Boot programs the outbound PCI window and a 256 MiB inbound identity
  DMA window. PCI config address is big-endian; config data is little-endian.
- 4 KiB mappings cover user virtual addresses in `[0x40000000, 0x80000000)`;
  refill searches the space's ordered records. TLB1 slots 3–15 cache user mappings. Real DTLB/ITLB
  misses refill from the current address space; switching spaces invalidates
  every user cache entry. The refill path does not restart the decrementer.
- Hardware UR/UW/UX permissions enforce supervisor isolation, read-only text
  and non-executable data. Writable/executable page requests are rejected.
- `sc` enters the real syscall vector, and `rfi` returns to the interrupted
  instruction stream. A hardware decrementer interrupt preempts busy loops.
- Every frame preserves scalar GPRs, LR/CTR/CR/XER, r2/r13 TLS state, USPRG0, all SPE
  upper GPR halves, the accumulator and SPEFSCR. Kernel SPE and SPRG context
  is restored before control returns to C++. No classic-FPU state is advertised.
- Native syscalls use r0 for the number, r3–r8 for arguments and signed r3 for
  the result. This project ABI does not use Linux's PowerPC CR0.SO errno flag.
- The backend owns IVPR/IVOR and the BookE translation cache on one boot CPU.
  Synchronous entry requires kernel interrupts masked; user MSR.EE enables the
  decrementer while applications execute. It does not advertise SMP execution or arbitrary PowerPC board support.

Run the independent hardware probes without changing xmake configuration:

```sh
python3 scripts/test_native_ppc.py --toolchain toolchains/powerpc-eabi
python3 scripts/test_native_ppc.py --toolchain toolchains/powerpc-eabi --kernel-fault
python3 scripts/test_ppc_tlb.py --qemu /usr/bin/qemu-system-ppc --kernel build/linux/ppc/debug/kernel.bin
```

The normal probe checks PR=1, syscalls and continuation, separate address spaces,
GPR/control/SPE/TLS preservation, kernel context restoration, decrementer
preemption, safe copies, supervisor and read-only faults, NX instruction fetch,
privileged instructions and TLB eviction across 20 distinct data pages. The NX
case first reads the data page to populate its TLB entry, then branches to it
and requires a real instruction-storage permission exception. The kernel-fault
probe requires a separate supervisor diagnostic before any user-frame pointer
or user stack is dereferenced.
