# SMP

`ok::smp::CpuTopology` tracks boot, starting, online, halted, and offline CPUs.
The kernel starts with the boot CPU online; secondary bring-up publishes the
CPUs it discovered and the ones that came online (`Kernel::publish_cpu_topology`).
x86_64, aarch64 and rv64 start secondaries and run native threads on them; the
other seven profiles report UP. `docs/qualification/SMP_BRINGUP.md` records the
results and limits.

`TopologyMode` describes the CPU topology policy:

- `single_core`: only the boot CPU is expected online.
- `symmetric`: multiple equivalent CPUs are available.
- `numa_aware`: CPU records carry NUMA domain placement for future allocators.

`SpinLock`, `ScopedSpinLock`, and `PerCpu<T>` provide the minimal primitives
needed by shared kernel modules without dynamic allocation. `SpinLock` is an
unranked leaf lock: test-and-test-and-set, pausing with the architecture's
`spin_pause` hint while it waits.

## CPU primitives and ranked locks

Every `NativeInstructions` profile provides `save_and_disable_interrupts`,
`restore_interrupts`, `spin_pause` and `cpu_index`. The save returns the mask
state at the kernel's privilege level (x86 IF, AArch64 DAIF.I, ARM32 CPSR.I,
RISC-V `mstatus.MIE` because the kernel runs in machine mode, LoongArch
CRMD.IE, MIPS Status.IE, PPC MSR[EE]) and restore re-enables only when that
state was enabled. Hosted builds save 0 and restore nothing. `cpu_index` is 0 on
profiles that run UP; AArch64 reads the logical id from the per-CPU block
`TPIDR_EL1` names (below). `ok::smp::current_cpu()` (`ok/smp/cpu.hpp`) returns
it. Hosted model tests instead set a thread's CPU with `ScopedHostedCpu`.

`RankedSpinLock<LockRank>` (`ok/smp/lock.hpp`) records its owner as CPU + 1. A
CPU takes ranked locks in strictly increasing `LockRank`. `RankedSpinGuard`
masks interrupts before acquiring and restores them after releasing;
`RankedSpinLock::Guard` is for callers that already masked them. Debug kernels
define `OK_LOCK_DEBUG`, which keeps a per-CPU record of up to eight holds and
reports these violations to `lock_rank_violation`: a rank not above every held
rank, a self-deadlock (reported before spinning; `lock()` returns without
the lock and its matching `unlock()` leaves the outer hold in place), an unlock by a CPU that does not own the lock, an unlock out of LIFO
order, and nesting past the record. `try_lock` skips the order check because it
cannot deadlock, but its hold is recorded. `lock_violation_handler` is null by
default, so violations are only counted per CPU. Release builds compile the
checks out.

`scripts/test_smp_locks.py` is a hosted model suite (scope=model) where host
threads stand in for CPUs. The debug test point `smp/ranked-lock-order` takes
two ranked locks on the boot CPU only. Neither shows AP startup, IPIs or
hardware SMP execution.

## CPU discovery

`ok/boot/cpu_topology.hpp` defines `CpuTopologyMap`, a constant-initializable
record of up to `smp::max_cpus` firmware-described CPUs (hardware id, enabled,
boot), the start mechanism the description supports and, when there is none,
the reason the platform must run UP. Parsing never starts a CPU.

- `parse_fdt_cpus` (`ok/boot/fdt.hpp`) reads `/cpus` (one or two address
  cells, zero size cells), each `device_type = "cpu"` child's `reg`, `status`
  and `enable-method`, `timebase-frequency` and a root-level PSCI node. The
  caller names the binding with `FdtCpuFlavor`. Arm selects `psci_hvc` or
  `psci_smc` only for PSCI 0.2/1.0 with an `hvc`/`smc` method and when every
  enabled secondary names `psci`; `cpu_on` defaults to the SMC64 id. RISC-V
  selects `riscv_msip` (M-mode release through the CLINT) for two or more
  enabled harts. The FDT header's `boot_cpuid_phys` marks the boot CPU when it
  names an enabled CPU.
- `find_acpi_rsdp` and `parse_acpi_madt` (`ok/boot/acpi.hpp`) read physical
  memory only through a `PhysicalReader` (function pointer plus context), so
  hosted tests supply synthetic memory. The XSDT is preferred for revision 2+
  RSDPs; root and MADT checksums and every length are checked against the
  reader's window. Enabled local APIC and x2APIC entries are present CPUs,
  online-capable ones are recorded disabled, others are skipped. Two or more
  enabled CPUs select `x86_sipi`; the caller identifies the BSP with
  `mark_boot_cpu`.

UP reasons are `single-cpu`, `psci-absent`, `psci-0.1-unsupported`,
`psci-method-missing`, `psci-method-unsupported`, `enable-method-not-psci` and,
from `mark_boot_cpu`, `boot-cpu-not-described`. A start method is usable only
once `boot_known` is set. More CPUs than `max_cpus` set `truncated`; the FDT
parser keeps the boot CPU in the map. `scripts/test_cpu_topology.py` is the
model suite; with `--qemu-dir` it also checks DTBs dumped by QEMU virt.

## AArch64 secondary CPUs

`src/arch/aarch64/smp.cpp` discovers CPUs from the device tree the boot
handoff kept (it must lie in the identity-mapped RAM 0x40000000-0x80000000),
with the PSCI conduit `/psci` names: `hvc` on plain QEMU virt, `smc` with
`virtualization=on`. With two or more CPUs it requires PSCI_VERSION 0.2 or
later through that conduit; a UP boot never calls the firmware. The boot CPU is
its own `MPIDR_EL1 & 0xff00ffffff`. The OK_SMP method is `psci-hvc` or
`psci-smc`.

`start` fills a `SecondaryStart` block (`secondary.hpp`: stack, per-CPU block,
MAIR, TCR, VBAR, CPACR, the kernel TTBR0, a fixed SCTLR of M, C, I, SA, SA0,
nTWI, nTWE and the ARMv8.0 RES1 bits, the C++ entry and the logical id), cleans
it to the point of coherency and issues CPU_ON with that block as the context.
`ok_aarch64_secondary_entry` (`boot.S`) masks DAIF, leaves EL2 through the same
`DROP_TO_EL1` macro as the boot entry, sets SP and `TPIDR_EL1`, and enables the
MMU before it touches any shared object; BSS is never cleared there.
`ok_aarch64_secondary_start` then resets the CPU's banked GIC state, records its
GIC interface number (the banked `GICD_ITARGETSR0` byte) and calls
`ok_smp_secondary_main`.

IPIs are SGI 1, written to `GICD_SGIR` with the target's interface bit after
`dsb ishst`; the receiver completes the full IAR value before it drains the
reasons. Each CPU's local timer is its banked CNTP (PPI 30): the execution
backend re-arms it per user entry and `wait_local` arms it one-shot at 100 Hz.
Device SPIs target CPU 0, and only CPU 0 runs their top halves,
`ok_interrupt_tick()` and `record_timer_latency()`; another CPU's timer
reports `TrapKind::timer` through `local_tick()`, and an IPI in user mode
reports `TrapKind::interrupt` on every CPU.

The execution backend keeps each entry's state (resume SP, entry stack top,
active frame, kernel SIMD area, `executing`, last trap) in a 64-byte aligned
`CpuBlock` that `TPIDR_EL1` names; `execution.S` reaches it relative to that
register. `boot.S` sets CPU 0's after clearing BSS and `initialize()` sets it
again for runners without that path. Only CPU 0 has the unowned diagnostic
entry stack; a secondary enters user code only through `enter_owned`.
`remap()` and `destroy_space()` call `quiesce_root` before tables change, and
code cache maintenance uses `ic ialluis`.

## Bring-up

`ok::smp::start_secondary_cpus` (`src/smp/bringup.cpp`) runs once on the boot
CPU, after the debug suite or right after kernel boot in release mode. It
discovers the CPUs through `ok/arch/smp.hpp` (each SMP profile's
`src/arch/<arch>/smp.cpp`; `src/arch/smp_stub.cpp` elsewhere). With more than
one, it initializes the execution backend and clock early, gives every enabled
secondary 256 KiB of frame-allocator stack and starts it. A secondary counts
online only once it ran `ok_smp_secondary_main` and published the hardware id
it read from its own registers; bring-up then sends each one an IPI and waits
for it. It prints, before `OK_TEST_PASS`:

```
OK_SMP scope=hardware cpus=<discovered> online=<online> ap_start=<method> ipi=<pass|fail|unsupported> [reason=<token>]
```

`ok/smp/bringup.hpp` keeps the per-CPU `CpuRecord`s header-inline (state,
started and reported ids, stack bounds, IPI reasons, counters) with
`send_ipi`, `receive_ipi`, `local_tick` and the user-window protocol:
`begin_user(root)` and `end_user()` bracket the time a CPU may run user code
under a translation root, and `quiesce_root(root)` sends every other CPU in
such a window an IPI and waits until the window closed. Backends call it
before a translation changes or its tables are freed.

Device interrupts, their top halves, `ok_interrupt_tick()` and the timer
latency histogram stay on CPU 0. `interrupt::wait_for_interrupt()` returns
false on any other CPU, so a driver that waits for its device inside a syscall
there polls the completion instead.

## x86_64 secondary CPUs

Discovery reads the ACPI MADT through the identity map (fw_cfg's CPU count
only bounds it) and marks the boot CPU by its own APIC id. A 16-bit trampoline
copied to 0x8000 takes each AP to long mode on the boot PML4 (INIT, 10 ms,
SIPI, a second SIPI only if it has not arrived). Every CPU has a per-CPU block
reached through `GS_BASE` with its own GDT, TSS, #DF stack and default rsp0
stack; user GS goes through `IA32_KERNEL_GS_BASE` and `swapgs` on every CPL3
transition. The xAPIC page is mapped uncached in the boot tables. The boot CPU
keeps the 8259 and PIT (LINT0 ExtINT, LINT1 NMI); IPIs use vector 0xF0 and each
AP's local timer is the LAPIC timer at 0xEF, calibrated once against the TSC.

## RISC-V harts

`docs/arch/rv64.md` describes the M-mode hart start: under `-bios none` every
hart enters `_start`; harts other than 0 park without a stack and are released
through their CLINT MSIP (`ap_start=clint-msip`), which also carries IPIs. tp
names the per-CPU block. rv32 keeps its secondary harts parked.

## Native threads on several CPUs

The kernel lock (`ok/smp/kernel_lock.hpp`, rank `big_kernel`) serializes all
kernel work; each CPU's loop releases it only while it runs user code or
idles. `src/user/runtime/smp.cpp` installs the shared capture's entry: it
records the CPU's entry, opens the user window, releases the lock, runs the
thread on a copy of its frame and takes the lock again; the capture then looks
the process and thread up again by id. A CPU that retires a thread running
elsewhere first takes that CPU's entry over (`evict_thread`). Secondaries
contend for the lock only once the runtime admitted them, and each runs
`ok_smp_secondary_step` from `ok_smp_secondary_main`.

With one CPU online `step()` keeps the `(pid, tid)` round robin. With
secondaries online it places every runnable thread that is off-CPU on the run
queue of its CPU (`enqueue_thread`), takes the next entry from this CPU's queue
or steals one (`dequeue_thread`), dispatches it with `mark_on_cpu` and sends an
idle CPU it handed work a reschedule IPI. The entry stack pool keeps one active
lease per CPU; the scheduler's no-argument current thread and activation act
for the calling CPU. With more than one CPU online the interrupt report ends
with `OK_SMP_CPUS scope=hardware online=N cpuK=<user entries>:<local
ticks>:<IPIs received>:<IPIs sent>:<shootdowns waited for>`.

