# SMP

`ok::smp::CpuTopology` tracks boot, starting, online, stopping, halted, and
offline CPUs. The kernel starts with the boot CPU online; secondary bring-up
publishes the CPUs it discovered and the ones that came online
(`Kernel::publish_cpu_topology`). x86_64, aarch64 and rv64 start secondaries,
run native threads and admitted syscalls on them, route device lines to them
and can stop and restart them; the other seven profiles report UP.
`docs/qualification/SMP_BRINGUP.md` records the results and limits.

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
CPU takes ranked locks in strictly increasing `LockRank`:

| Rank | Name | Instances |
| --- | --- | --- |
| 10 | `big_kernel` | the kernel lock |
| 14 | `ofd_position` | one per open file description |
| 16 | `fs_provider` | the ext4 operation gate's recorded hold |
| 20-60 | `scheduler`, `run_queue`, `wait_queue`, `ipi`, `tlb` | reserved |
| 64 | `arch_spaces` | the x86_64, aarch64 and rv64 address-space slot locks |
| 70 | `memory` | reserved |
| 80 | `irq_router` | the platform interrupt router |
| 82 | `realtime_clock` | the realtime anchors |
| 84 | `monotonic_clock` | clock initialization; samples on the UP profiles |
| 90 | `console` | the runtime's console writer |
| 255 | `leaf` | |

`RankedSpinGuard` masks interrupts before acquiring and restores them after
releasing; `RankedSpinLock::Guard` is for callers that already masked them.
Debug kernels define `OK_LOCK_DEBUG`, which keeps a per-CPU record of up to
eight holds and reports these violations to `lock_rank_violation`: a rank not
above every held rank, a self-deadlock (reported before spinning; `lock()`
returns without the lock and its matching `unlock()` leaves the outer hold in
place), an unlock by a CPU that does not own the lock, an unlock out of LIFO
order, and nesting past the record. A successful `try_lock` is order checked
and recorded like `lock()`; a failed one records nothing. A gate that is not a
`RankedSpinLock` (an owner word under an unranked lock) records its hold with
`lock_debug_may_wait`, `lock_debug_hold` and `lock_debug_release` after it
refused same-CPU reentry itself. `lock_violation_handler` is null by default,
so violations are only counted per CPU; the native runtime installs a recorder
that keeps the first one per CPU. Release builds compile the checks out.

Every CPU has a mode word (`cpu_modes`), written only by that CPU:
`BKL_HELD` (holds the kernel lock), `SECTION` (runs a kernel section),
`SPINNING` (waits) and `KERNEL_WORK` (its kernel-lock stretch serves work).
`SpinningScope` sets `SPINNING` for the duration of a wait and counts it; a
nested scope leaves the outer bit alone. Contended lock waits, ext4 gate,
position lock and drain waits, the interrupt-serving relock and
`wait_for_interrupt()` halts that keep the kernel lock all run inside one.
`spin_service`, installed by the runtime where device lines reach secondary
CPUs, lets a gate or position-lock wait serve the CPU's pending interrupts.

`scripts/test_smp_locks.py` is a hosted model suite (scope=model) where host
threads stand in for CPUs; it covers the ranks, the `try_lock` order check and
the gate helpers. The debug test point `smp/ranked-lock-order` takes two
ranked locks on the boot CPU only. Neither shows AP startup, IPIs or hardware
SMP execution.

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
  `mark_boot_cpu`. Given a `MadtInterrupts` record it also keeps the I/O APIC
  entries and the ISA interrupt source overrides; a malformed one marks that
  record rejected and empty while the CPU map still parses
  (`docs/modules/interrupts.md`).

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
Device SPIs are routed to any online CPU (below); `ok_interrupt_tick()` and
`record_timer_latency()` run on CPU 0 only. Another CPU's timer reports
`TrapKind::timer` through `local_tick()`, and an IPI in user mode reports
`TrapKind::interrupt` on every CPU.

The execution backend keeps each entry's state (resume SP, entry stack top,
active frame, kernel SIMD area, `executing`, last trap) in a 64-byte aligned
`CpuBlock` that `TPIDR_EL1` names; `execution.S` reaches it relative to that
register. `boot.S` sets CPU 0's after clearing BSS and `initialize()` sets it
again for runners without that path. Only CPU 0 has the unowned diagnostic
entry stack; a secondary enters user code only through `enter_owned`.
`remap()` and `destroy_space()` call `quiesce_root` before tables change, and
code cache maintenance uses `ic ialluis`. User entries run under per-CPU ASIDs
(see "User TLB tags"); the only broadcast `tlbi vmalle1is` is the one in
`initialize()`. `spaces_lock` (rank `arch_spaces`) guards the slot table
against the entry path's lookup, which runs after the kernel lock was dropped.

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
OK_SMP scope=hardware cpus=<discovered> online=<online> ap_start=<method> ipi=<pass|fail|unsupported> [reason=<token>] [tsc=<sync|adjusted|offset>]
OK_SMP_TLB scope=hardware asid=<bits|none> pcid=<on|off>
```

`tsc=` appears on x86_64 once a secondary started: the worst TSC
synchronization result over the online secondaries (below). When the three
SMP profiles discovered more than one CPU, `OK_SMP_TLB` follows: the
narrowest user TLB tag width among the started CPUs (`none`: every user entry
flushes the user TLB) and whether x86 PCIDs carry the tags. A debug kernel
with CPU 1 online then runs the hotplug test point (`OK_SMP_HOTPLUG_BOOT`, see
"CPU hotplug").

`ok/smp/bringup.hpp` keeps the per-CPU `CpuRecord`s header-inline (state,
started and reported ids, stack bounds, IPI reasons, counters) with
`send_ipi`, `receive_ipi`, `local_tick` and the user-window protocol:
`begin_user(root)` and `end_user()` bracket the time a CPU may run user code
under a translation root, and `quiesce_root(root)` sends every other CPU in
such a window an IPI and waits until the window closed. Backends call it
before a translation changes or its tables are freed. `started_mask()` names
the CPUs bring-up started; `online_mask()` the ones online now. `send_ipi`
and `quiesce_root` never send to or wait for a CPU outside the online mask.

## x86_64 secondary CPUs

Discovery reads the ACPI MADT through the identity map (fw_cfg's CPU count
only bounds it) and marks the boot CPU by its own APIC id. A 16-bit trampoline
copied to 0x8000 takes each AP to long mode on the boot PML4 (INIT, 10 ms,
SIPI, a second SIPI only if it has not arrived). Every CPU has a per-CPU block
reached through `GS_BASE` with its own GDT, TSS, #DF stack and default rsp0
stack; user GS goes through `IA32_KERNEL_GS_BASE` and `swapgs` on every CPL3
transition. The xAPIC page and the I/O APIC page are mapped uncached in the
boot tables. IPIs use vector 0xF0 and each AP's local timer is the LAPIC timer
at 0xEF, calibrated once against the TSC. Once the interrupt service starts on
the I/O APIC, the 8259s and the boot CPU's LINT0 are masked and the PIT
reaches the boot CPU through its GSI; without a usable I/O APIC the boot CPU
keeps the 8259 and PIT (LINT0 ExtINT, LINT1 NMI).

### TSC synchronization

Before a started AP enters `ok_smp_secondary_main` it runs a handshake with
the boot CPU over its own cache-line channel (`src/arch/x86_64/smp.cpp`, with
the pure decision logic in `tsc_sync.hpp`). Both sides read the TSC behind an
MFENCE. The boot CPU measures in 64-round ping-pong batches, up to 16 batches,
until the tightest round bounds the offset within 50 us; the estimate is that
round's midpoint, its error half the round's width. An offset beyond the error
plus a 100 ns slack is corrected through `IA32_TSC_ADJUST` when both CPUs have
it, otherwise (or when that did not verify) through a per-CPU offset the clock
read subtracts, and every correction is verified by measuring again. A CPU that
cannot be verified (no answer, an unanswered correction, a bound that stays too
wide, or a residual beyond the tolerance) is never admitted: it finds its
channel closed and parks with interrupts masked before it reads the clock, and
bring-up marks it offline.

The APs share one start block in the trampoline page. It names the APIC id it
was written for (stored as id + 1, so a cleared block matches no CPU), and the
trampoline compares that with the CPU's own CPUID initial APIC id before it
loads the stack, halting on a mismatch: a late AP never runs on another CPU's
stack or per-CPU block. A CPU bring-up or a restart gives up on after
`start()` launched it (an unverified clock, never online, or online with
another id) gets INIT through `arch::smp::abandon()` and counts as parked, so
the page can be rewritten and the CPU restarted later. aarch64 and rv64 cannot
stop another CPU: there an abandoned CPU idles outside the online mask and is
not restarted.

Each AP prints one line before `OK_SMP`:

```
OK_TSC_SYNC scope=hardware cpu=<n> result=<sync|adjusted|offset|unsynced> invariant=<0|1> tsc_adjust=<0|1> slack=<cycles> max_error=<cycles> offset=<cycles> error=<cycles> adjusted=<cycles> software=<cycles> residual=<cycles> residual_error=<cycles> tolerance=<cycles>
```

The x86_64, aarch64 and rv64 monotonic clocks convert the counter without a
lock and never return less than a shared floor (`SharedCounterClock`), the
highest value returned so far on any CPU: a CPU whose counter trails reads
the floor until it catches up.

The debug-only option `x86_tsc_skew_test` makes CPU 1 rewind its TSC before
the handshake (`skew_test=tsc|software` in its line; QEMU TCG ignores the
`IA32_TSC` write, so the rewind goes into the software offset instead), and
`scripts/qemu_test.py --tsc-skew-test` then requires it to be detected and
corrected. `scripts/test_tsc_sync.py` is the hosted model of the estimator and
the correction decision.

## RISC-V harts

`docs/arch/rv64.md` describes the M-mode hart start: under `-bios none` every
hart enters `_start`; harts other than 0 park without a stack and are released
through their CLINT MSIP (`ap_start=clint-msip`), which also carries IPIs. tp
names the per-CPU block. rv32 keeps its secondary harts parked.

## User TLB tags

Each CPU tags the user translations it caches; tag 0 is the kernel's
translation.

- aarch64: ASIDs in TTBR0_EL1[63:48]. Each CPU sets TCR_EL1.AS from its own
  `ID_AA64MMFR0_EL1.ASIDBits`; an AP starts with 8-bit ASIDs and widens them in
  `prepare_local_tlb_tags()`, followed by a local `tlbi vmalle1`.
- rv64: satp ASIDs, with ASIDLEN probed on each hart.
- x86_64: 12-bit PCIDs. `load_cpu` sets CR4.PCIDE when CPUID.1:ECX[17] is
  set; the AP trampoline runs without PCIDE.

`src/arch/tlb_tags.hpp` keeps one 32-slot `TlbTagCache` per CPU. There is no
global allocator, and an entry needs no lock or handshake.

- Every space carries a `TlbSpaceTag`. Its id is never reused. Its generation
  moves when `remap` rewrote a translation, under `spaces_lock`, after
  `quiesce_root` and after the PTE write.
- On entry, a cached tag at the same generation keeps its entries. A stale
  generation, or a tag handed to a new space, invalidates that one tag on that
  CPU before user code runs: `tlbi aside1` while TTBR0 still selects ASID 0
  (aarch64), `sfence.vma zero, asid` (rv64), a CR3 load without bit 63 (x86).
- When all tags are in use, the next new space starts a rollover: tags are
  handed out again from the first, each invalidated as it is reassigned.
- Exit returns to the kernel translation without invalidating. CPUs inside a
  user window are covered by `quiesce_root`; CPUs that ran a space earlier
  catch up at their next entry.
- A destroyed space's leftover entries, walk-cache entries included, are never
  used again: hardware uses only the current tag, and a tag becomes current for
  another space only after it was invalidated.
- Untagged CPUs (x86 without PCID, rv64 with ASIDLEN 0) flush the user TLB on
  every entry.

`scripts/test_tlb_tags.py` is the hosted model of the protocol. The x86_64,
aarch64 and rv64 native probes run a tag churn past a rollover and a remap,
and with two CPUs a secondary re-enters a space after a remap shootdown and
must see the new frame (`remote_remap=pass`).

## Device interrupts on several CPUs

aarch64 (GICv2), rv64 (PLIC) and x86_64 (I/O APIC) route each bound device
line to the (line mod N)-th online CPU, and a line follows a device waiter on
another CPU. Any CPU runs top halves from its user trap path, its idle wait and
while it spins for the kernel lock (`lock_kernel_serving_interrupts()`);
bottom halves run under the kernel lock. A driver waiting for its device
inside a syscall on a secondary CPU halts there until its line, an IPI or the
local tick arrives, still holding the kernel lock; inside a kernel section it
polls. The global tick, `ok_interrupt_tick()` and the timer latency histogram
stay on CPU 0. `docs/modules/interrupts.md` describes the router, the wake
protocol and `OK_SMP_IRQS`.

## The kernel lock and kernel sections

### The kernel lock

The kernel lock (`ok/smp/kernel_lock.hpp`, rank `big_kernel`) serializes
kernel work; each CPU's loop releases it while it runs user code, idles or
runs a kernel section. It is taken only through `bkl_lock`, `bkl_try_lock`,
`bkl_unlock` and `BklGuard`, which keep the CPU's `BKL_HELD` bit and its
`acquired`, `contended` and `done` counters. `bkl_mark_work()` marks the
current stretch as work (`KERNEL_WORK`): `step()` chose a thread, a user trap
retook the lock, bottom halves ran, a section result was committed, or the
boot loop did work. `done` advances at the release of a marked stretch and at
every section end, so an idle CPU that takes the lock to scan an empty run
queue counts as neither.

State that CPUs touch without the kernel lock has its own lock:

- the console writer (`emit`, `emit_line`), rank `console`;
- the realtime anchors, rank `realtime_clock`, on every access; a reading and
  `clock_settime` (`arch::set_realtime_now`) take their monotonic sample
  inside it;
- clock initialization, and every sample on the UP profiles, rank
  `monotonic_clock`; x86_64, aarch64 and rv64 sample lock-free above the
  shared floor;
- the address-space slots, rank `arch_spaces`. x86_64, aarch64 and rv64 keep
  a fixed slot table (`fixed_space_slots`: processes, quarantine and one per
  CPU) whose storage never moves or is freed, so a lookup without the kernel
  lock is safe.

### Kernel-lock contracts

Debug kernels check two contracts (`ok/smp/kernel_section.hpp`), armed by the
runtime just before it admits secondary CPUs:

- `check_exclusive(site)` (`require_exclusive`) marks kernel-only state: the
  scheduler tables and wait registration, the object, native page and entry
  stack pools, the syscall origin and VFS stat continuation tables,
  `syscall::Table::dispatch` and `VirtualFileSystem::enter_operation`. It
  counts `exclusive_asserts` inside a kernel section and, once armed, without
  the kernel lock. Interrupt binding, bottom-half drains and exec's eviction
  commit call `require_exclusive` directly, in every build.
- `check_drained(pid, site)` (`require_drained`) marks the mutators of a
  process's footprint: installing and closing descriptors,
  `create_thread_prepared`, `exit_thread`, `fork_process`, `replace_image`,
  `finish_process`, `clear_current`, `clear_on_cpu`, `replace_native_pages`,
  copy-on-write resolution and exec before eviction. It counts
  `drain_asserts` while a section of that process is working or returning.

`PosixService` counts a fall-back to the boot resources inside a section or
during an armed dispatch for a user caller, and `occupy_cpu` counts a CPU
filling another CPU's slot. Release builds compile the annotations out.

### Kernel sections

A kernel section runs one admitted Linux syscall body with the kernel lock
released (x86_64, aarch64, rv64; build option `smp_sections`, default on,
defines `OK_SMP_SECTIONS`; it is off with the debug private futex peer
fixture). Each CPU has one `SectionRecord`:

1. idle to working: the pin, set only while holding the kernel lock, which is
   then released;
2. working to returning: the result is published and the CPU retakes the
   kernel lock, serving its interrupts while it waits;
3. returning to committed: under the kernel lock, by the owner or by a CPU
   that drained it first (a handoff);
4. committed to idle, by the owner.

`drain(pid)` and `drain_all()` are called holding only the kernel lock (depth
1): they wait for working sections and commit returning ones through
`section_commit_handler`. `drain_cpu(cpu)` does the same for one CPU before it
leaves (hotplug).

`step()` (`src/user/runtime/entry.cpp`) offers a freshly trapped syscall to
`admit_section` (`src/user/runtime/section.hpp`) under the lock. Converted
families: `getpid`, `gettid`, `clock_gettime` and `clock_getres` (clocks 0
and 1, time32 and time64 forms), and `read`, `write`, `pread64`, `pwrite64`,
`lseek`, `fsync`, `fdatasync` on a description whose node reports
`concurrent_io()` (only `Ext4Node`). Admission declines with no effect when
admission is closed or shutdown pending, the runtime failed, the caller is
not Linux, the number is not converted, something else claims the call, the
caller waits for the console, or the descriptor, node kind, `O_APPEND`, size
(64 KiB per call) or argument range disqualify it, or the user range does not
map. It resolves copy-on-write for an output range before the pin. The first
of every eight admitted identity calls on a CPU is declined as a shadow: the
body's result is computed and compared with the kernel-lock path's answer
(`shadow=<checked>/<mismatched>`). Debug kernels compare a digest of what a
decline must leave alone (`purity_faults`); a description's offset is part of
it only while no other CPU's section is working.

The body is the kernel-lock path's own helper (`ok/user/linux_file_io.hpp`)
with the caller bound in `PosixService` for that CPU. It touches only its own
process's footprint, read-only, and state behind fine locks:

- **Description position lock.** Each open description has an `ofd_position`
  lock, held in both modes across every offset-dependent read, write, seek and
  directory read together with its provider call, and around
  `F_GETFL`/`F_SETFL`. A node-backed `read(2)` staged in 4 KiB chunks holds
  it across every chunk and the copy-outs between them
  (`PosixService::read_held`), so it reads one contiguous range while other
  CPUs read the description. No path holds two. A section waiting for one serves its
  CPU's interrupts; one that would re-enter a lock its CPU holds reports a
  self-deadlock and a section assertion instead of spinning.
- **ext4 operation gate.** One operation at a time per filesystem, across
  CPUs. The owning CPU re-entering gets `busy`; another CPU waits, marked
  SPINNING and serving its interrupts, then repeats its identity checks. The
  hold is recorded at rank `fs_provider`
  (`docs/modules/ext4-storage.md`).
- **Device waits** inside a section poll: `wait_for_interrupt()` returns
  false, `drain_platform_irqs()` runs nothing and `bind_platform_irq()`
  refuses (the last two count `exclusive_asserts`); `OK_IRQ_COMPLETIONS`
  counts the wait as polled. Before returning false, `wait_for_interrupt()`
  serves the CPU's own controller, and the OS poll loop calls
  `smp::serve_while_polling()` on every pass, so top halves of lines routed to
  a polling CPU still run and wake a kernel-lock waiter elsewhere; bottom
  halves stay under the lock.
- The copy-on-write resolver refuses inside a section (`cow_refused`).

Drains run before every other syscall of the process (the choke point in
`step()`), in `finish()`, `Kernel::kill_process`, the copy-on-write resolver
(outside a section), and as `drain_all()` in `supervise_daemons`, reap
compaction, an `F_SETFL` that changes `O_APPEND`, and power-off preparation,
which closes admission first.

### Evidence lines

Once the runtime armed its checks, the interrupt report ends with three lines
on every profile that compiles sections in, with one CPU too, after draining
every section:

```
OK_SMP_KERNEL scope=hardware debug=<0|1> online=<started CPUs> sections= completed= declined= handoffs= concurrent= witnessed= max_parallel= spun= drains= drain_waits= bkl_acquired= bkl_contended= lock_violations= exclusive_asserts= drain_asserts= depth_asserts= section_asserts= cow_refused= purity_faults= shadow=<checked>/<mismatched> cpuK=<sections>:<concurrent>:<witnessed>:<bkl acquired>:<bkl contended> ...
OK_SMP_KERNEL_FAMILIES scope=hardware getpid= gettid= clock_gettime= clock_getres= read= write= pread64= pwrite64= lseek= fsync= fdatasync=
OK_SMP_KERNEL_DECLINES scope=hardware runtime= personality= number= pending= console= descriptor= kind= append= size= fault= shadow= shutdown=
```

A CPU is productive when it is in a section, or holds the kernel lock in a
stretch marked as work, and is not SPINNING; a section's own device polling
counts as section work. Overlap is counted only for sections that never
waited (otherwise `spun`): `concurrent` when another CPU was productive at
the section's end, `max_parallel` as 1 plus the productive others at that
sample, and `witnessed` when another CPU's `done` moved during the section.
`lock_violations` counts violations since arming; the assertion counters and
the shadow mismatches must be 0 in a debug gate. A non-zero count adds
`OK_SMP_KERNEL_VIOLATION` and `OK_SMP_KERNEL_ASSERT` lines naming the first
violation and site per CPU.

`scripts/test_kernel_section_protocol.py`, `scripts/test_ext4_gate_concurrency.py`
and `scripts/test_linux_file_io_equivalence.py` are hosted model tests of the
protocol, the gate and the bodies with admission; none runs a guest.

## Native threads on several CPUs

`src/user/runtime/smp.cpp` installs the shared capture's entry: it records
the CPU's entry, opens the user window, releases the kernel lock, runs the
thread on a copy of its frame and takes the lock again, serving its
interrupts while it waits; the capture then looks the process and thread up
again by id. Secondaries contend for the lock only once the runtime admitted
them, and each runs `ok_smp_secondary_step` from `ok_smp_secondary_main`.

A CPU that retires a thread running elsewhere first takes that CPU's entry
over: `prepare_eviction` plans the CPUs and validates their active leases, and
`commit_eviction` quiesces the address space, deactivates each lease, clears
the thread's CPU and marks the entry revoked (`evict_thread` does both for one
thread). Exec makes every step that can refuse it first (scheduler
`preflight_replace_image`, the entry stack plan, the eviction plan and lease
validation, VFS retirement), so a refused exec leaves siblings running. The
futex, UDP, syscall-origin and VFS-continuation preflights that follow
predict invariant breaches and stop the runtime, as a failure after
`commit_eviction` does, without resuming the caller. An exec that evicted
entries prints `OK_NATIVE_EXEC_EVICT pid= tid= evicted=N`.
`scripts/test_native_exec_eviction.py` runs exec with a sibling on a modelled
second CPU through the actual runtime on all ten profiles (scope=model).

With one CPU online `step()` keeps the `(pid, tid)` round robin. With
secondaries online it places every runnable thread that is off-CPU on the run
queue of its CPU (`enqueue_thread`), takes the next entry from this CPU's queue
or steals one (`dequeue_thread`), dispatches it with `mark_on_cpu` and sends an
idle CPU it handed work a reschedule IPI. The entry stack pool keeps one active
lease per CPU; the scheduler's no-argument current thread and activation act
for the calling CPU. With more than one CPU started the interrupt report ends
with `OK_SMP_CPUS scope=hardware online=N cpuK=<user entries>:<local
ticks>:<IPIs received>:<IPIs sent>:<shootdowns waited for>`, where N and the
listed CPUs are the ones bring-up started.

## Futex user words

Futex user words are updated by atomic operations on the frame behind the
word (`src/user/futex_word.hpp`). `arch::execution::user_word` resolves an
aligned user address to its kernel pointer; a write first gets a private
writable copy-on-write frame, as a user store would. Each operation is one
sequentially consistent atomic: FUTEX_WAKE_OP's update of the second word is
one read-modify-write, FUTEX_WAIT, WAIT_BITSET and CMP_REQUEUE compare with
one load, robust-list retirement loads and compare-exchanges the owner word,
and `clear_child_tid` is one store. The kernel lock still serializes the futex
queues, so a compare and the enqueue or requeue that follows it cannot be
split by another futex call. i386 has no CMPXCHG and starts no secondary CPU,
so it uses plain aligned accesses; the build refuses that fallback on a
profile that defines `OK_ARCH_SMP_START`. The SDK profiles
`kernel-lock-atomic-word-...-v2` (wake-op) and
`kernel-lock-atomic-compare-...-v2` (requeue) export `smp_atomic = 1`: atomic
with respect to every CPU that can run user code on the profile.
`scripts/test_futex_word.py` races the primitives against host threads
(scope=model).

## CPU hotplug

`smp::stop_secondary_cpu(kernel, cpu)` and `smp::restart_secondary_cpu(kernel,
cpu)` take a started secondary CPU offline and back (x86_64, aarch64, rv64;
the other profiles return `unsupported`). CPU 0 never leaves. The caller holds
only the kernel lock and gets it back; both calls release it while they wait,
and one request runs at a time.

Stop marks the CPU `stopping`, removes it from the scheduler's online set and
moves its queued threads (`SchedulerState::offline_cpu`), drains its kernel
section (`drain_cpu`) and kicks it out of user mode. At its next loop pass,
outside user code and holding nothing, the CPU:

- takes the kernel lock and clears its runtime per-CPU state
  (`ok_smp_cpu_leaving`);
- leaves the online mask, so IPIs and shootdowns stop covering it and every
  routable controller refuses it as a target;
- moves its device lines away (`retarget_platform_irqs_away`);
- releases the lock, serves what its controller already accepted, stops its
  local timer and clears `ipi_pending`;
- parks: x86 halts with interrupts masked (`cli; hlt`) until INIT; aarch64
  turns its GIC CPU interface off and calls PSCI CPU_OFF, which the requester
  confirms with AFFINITY_INFO; rv64 returns to the `boot.S` park loop through
  `ok_rv64_hart_park`, which clears its mailbox entry.

The requester waits up to 2 s for the park; on a timeout it marks the
topology entry halted, so `Kernel::tick` does not schedule on that CPU.
Restart reuses the CPU's stack and the architecture's start path (x86 repeats
INIT-SIPI and the TSC handshake, printing no second `OK_TSC_SYNC` line), waits
for it to come online with its reported id, puts it back in the online mask,
requires an IPI answer and then calls `scheduler.online_cpu`. Device lines are
not rebalanced onto it. Nothing a parked CPU could touch is freed, and
`OK_SMP_CPUS`, `OK_SMP_IRQS` and `OK_SMP_KERNEL` keep listing it with its
cumulative counters.

Nothing outside tests requests hotplug:

- The debug boot test point stops CPU 1, raises an IPI at it while parked
  (which must not be taken), restarts it and requires an IPI answer:
  `OK_SMP_HOTPLUG_BOOT scope=hardware cpu=1 stop=<pass|fail>
  parked_ipi=<pass|fail> restart=<pass|fail> ipi=<pass|fail>`.
- The debug-only option `smp_hotplug_test` makes CPU 0 cycle the highest
  started secondary every 100 ms while native userland runs. Every interrupt
  report then repeats a cumulative `OK_SMP_HOTPLUG scope=hardware
  cycles=<n> failures=<n> cpu=<id>`; the last one is the verdict.

## What stays serialized

Under the kernel lock, on every CPU:

- thread selection, user entry, capture, write-back and per-step housekeeping;
- every blocking call and its resume;
- `open`/`openat`, `close`, `dup`, `fcntl`, `stat`, `rename`, `unlink`,
  `mkdir`, `getdents` dispatch, `chdir` and the whole VFS namespace;
- pipes, AF_UNIX, PTYs and the console;
- fork, clone, exec, exit, wait, kill, signals and timers;
- futex queues (the user-word operations themselves are atomic);
- mmap, mprotect, munmap, brk, memfd and copy-on-write faults;
- UDP, TCP, getrandom, module load, power and the rescue GUI;
- device bottom halves and interrupt binding.

Other limits:

- `O_APPEND` writes and every node other than ext4 stay under the kernel lock.
- ext4 runs one operation at a time per filesystem, so two file sections
  never do disk work at the same moment; they overlap with kernel-lock work
  and identity and clock sections.
- Each description serializes its own offset-dependent I/O.
- A process's kernel-lock syscalls wait for that process's sections, and every
  reap and every `O_APPEND` change waits for all sections.
- A driver waiting for its device under the kernel lock keeps the lock while it
  halts.
- Only the Linux personality on x86_64, aarch64 and rv64 runs sections; the
  seven UP profiles take the same locks uncontended.
