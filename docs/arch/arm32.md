# arm32

The profile boots the standalone kernel on QEMU `virt` with the existing
`cortex-a15` CPU and an `arm32-linux-image` entry at `0x40010000`. The build uses little-endian
ARMv7-A, ARM instruction mode and the soft-float C/C++ ABI. Native execution
requires the CPU's VFP/NEON register bank, generic timer and GICv2.

## Native execution

`src/arch/arm32/execution.cpp` and `execution.S` implement User-mode entry,
SVC return, timer preemption and fault capture. Initialization requires boot
CPU 0 in SVC mode with the MMU and data cache disabled. The bridge temporarily
installs its vector table and short-descriptor page tables, then restores the
kernel's CP15 state and exception-mode stacks on return.

Each address space has a 16 KiB-aligned L1 table (a four-page table-storage
run) and one table-storage page per populated 4 MiB holding its four coarse L2
tables, all in the RAM sections and released with the space. Supplied 4 KiB
backing pages are mapped in `0x10000000 <= address < 0x40000000`; the slot table
grows with the number of spaces. AP/APX
permissions and XN enforce user page permissions. Mapping validation rejects
writable executable pages, duplicates, bad alignment and invalid ranges.
Copies validate the entire range before accessing the physical backing, and
generations reject stale handles.

Platform MMIO below `0x10000000` is mapped privileged-only and non-executable.
Kernel RAM at `0x40000000..0x50000000` is identity-mapped privileged-only.
Native mappings are noncacheable, and caches stay disabled during user entry;
TLB, instruction cache and branch predictor invalidation accompanies a switch.
These mappings target the current QEMU board, not arbitrary ARM hardware.

The bridge saves user r0-r15, CPSR condition flags and Thumb IT state,
TPIDRURW/TPIDRURO, d0-d31 and FPSCR. Kernel callee-saved integer and VFP registers,
TLS, FPU controls and banked exception state are restored. ARM/Thumb state is
selected by bit 0 of the initial entry address. The current OS CRT uses ARM
mode. Syscalls use `svc #0`, r7 for the number, r0-r5 for arguments and r0 for
the result.

GICv2 delivers physical generic timer PPI 30 at approximately 10 ms intervals.
The backend saves/restores the timer compare/control, GIC interface controls,
priority and enable state. It owns this PPI while running native code and
clears its pending state when entering/leaving a quantum. Other device IRQs
are not serviced by this native trap bridge.

## Hardware validation

From the kernel repository:

```sh
python3 scripts/test_native_arm32.py
```

The isolated freestanding probe builds into `build/native-tests/arm32` without
changing xmake configuration. `--toolchain`, `--qemu` and `--output` override
tool paths and output location. It writes `probe.elf` and `serial.log`, requires
QEMU to exit successfully and checks `ARM32_NATIVE_PASS scope=hardware`.
Semihosting is enabled only for the probe's kernel completion signal.

The probe executes real User-mode SVCs and returns, preserves user/kernel TLS,
resumes a Thumb IT instruction, checks timer preemption and continued register
state, and provokes read-only, XN and kernel-address faults. It verifies
full-width d8/d31 state and kernel callee-saved VFP restoration. A-to-B-to-A
switches execute actual user loads/stores with different physical backing at
the same virtual address. Safe-copy boundaries, W^X policy, invalid stacks and
stale handles are also checked. Expected syscall/fault checks tolerate bounded
timer preemption.

The regular xmake QEMU kernel suite and OS ELF32/application integration tests
remain separate. This probe does not establish full Thumb application support,
POSIX completeness, device IRQ delivery or arbitrary board compatibility.

## Current limits

- Only boot CPU 0 executes user code; there is no native SMP or nested entry.
- The backend requires SVC boot; HYP-mode boot and TrustZone transitions are not
  implemented.
- Short descriptors, fixed backing/page-table capacities and the QEMU memory
  layout are used; LPAE, demand paging and copy-on-write are not implemented.
- Native execution uses uncached mappings and is not a cache/performance tuning
  milestone.

The boot platform separately provides PL011 UART, ramfb output and virtio-mmio
keyboard/mouse support.

## Firmware image contract

SDK 13 supplies an uncompressed ARM raw image with Linux zImage magic at offset
0x24, linked start at 0x28 and exact emitted file end at 0x2c. The initial ARM
branch must land after the 48-byte header within the payload. The producer and
OS consumer both validate format/architecture association, branch, magic and
exact file length. The complete linked memory end is independently reserved and
must fit below `0x50000000`. The ELF remains a diagnostic artifact; directly
launching it supplies no FDT and fails process-memory admission.

QEMU's 512 MiB recipe supplies r0=0, r1=0xffffffff and r2 FDT. Entry state and
bounded strict FDT parsing precede qualification; r3 is retained but unspecified.
The 64 KiB boot prefix, complete image/NOLOAD and full FDT storage are reserved.
Actual user-frame eligibility is restricted to the 256 MiB supervisor RAM map.
The isolated execution test still launches its diagnostic ELF deliberately; it
has no process allocator or firmware admission and is not a firmware test.

Unsupported CPSR.E or SCTLR.EE endian state is rejected in assembly before stack,
BSS and capture stores. The exported predicate and receipt consumers recheck
both bits; permissible I-cache state remains unchanged.
