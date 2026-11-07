# SMP bring-up

WT2.1 `feat/smp-bringup`. Slice 1: CPU primitives, ranked locks, per-CPU run
queues, CPU discovery parsers and the QEMU `-smp` lane. Slice 2: secondary CPU
start, IPIs, local timers and per-CPU execution state on x86_64, aarch64 and
rv64. Slice 3: the native runtime serves threads on every online CPU. Slice 4:
the concurrent filesystem/socket/fork stress gate. The follow-up ("Lifting the
bring-up limits") reorders exec, makes futex user words atomic, synchronizes
x86 TSCs, tags user TLB entries, routes device lines to every CPU, runs
admitted syscalls as kernel sections and adds CPU hotplug. The Design and
slice sections record the state at slice 4; `docs/modules/smp.md` describes
the current behaviour.

## Design

- **One kernel lock, parallel user code.** There are no kernel threads: each
  CPU's kernel loop enters user code through `execution::enter_owned` and gets
  control back on a syscall, tick, fault or interrupt. Every CPU holds the big
  kernel lock (`ok/smp/kernel_lock.hpp`, rank `big_kernel`) while it touches
  kernel state and releases it only while it runs user code or idles. Kernel
  work is serialized; user code of different threads runs concurrently. Finer
  locks rank after it (`ok/smp/lock.hpp`); debug kernels check the order.
- **Bring-up** (`src/smp/bringup.cpp`, `ok/arch/smp.hpp`). After the debug
  suite, or right after kernel boot in release mode, the boot CPU discovers the
  CPUs (ACPI MADT on x86_64, the FDT `/cpus` and `/psci` nodes on aarch64 and
  rv64), starts every enabled secondary on 256 KiB of frame-allocator stack,
  and counts it online only once that CPU ran kernel C++ code and published
  the hardware id it read from its own registers. It then sends each one an
  IPI and waits for the acknowledgement. The result is one line, printed before
  `OK_TEST_PASS`:
  `OK_SMP scope=hardware cpus=<discovered> online=<online> ap_start=<method> ipi=<pass|fail|unsupported>`
  with `reason=<token>` when the machine runs UP. With one CPU discovered
  nothing else changes: the backend is not initialized early, no interrupt
  controller is touched.
- **Honest topology.** The kernel no longer marks the profile's hardware thread
  count online at boot. It starts with the boot CPU and bring-up publishes the
  CPUs that came online (`Kernel::publish_cpu_topology`), so `OK_TEST_PASS
  cpus=` is the online count.
- **Device interrupts stay on CPU 0.** Top halves, the tick that drives
  `ok_interrupt_tick` and the timer latency histogram are boot-CPU state.
  Secondaries take only their local timer (a user tick that returns
  `TrapKind::timer`) and IPIs (`TrapKind::interrupt`).
- **Running user code on another CPU.** The shared capture enters the backend
  through `enter_unlocked` (`src/user/runtime/smp.cpp`): it records the CPU's
  entry, opens the CPU's user window, releases the kernel lock, runs the
  thread on a copy of its frame and takes the lock again. The process and
  thread records are then looked up by id, because another CPU may have moved
  or retired them. A CPU that retires a thread running elsewhere (exit, group
  exit, exec of a sibling) first sends that CPU out of user mode, takes over
  its entry stack lease and marks its entry revoked; the CPU discards the
  entry when it returns.
- **TLB shootdown.** A CPU's user window is open (odd `user_generation`)
  from before it loads a user translation root until after it switched back
  to the kernel root (at slice 4 this discarded the user TLB on every
  profile; see "User TLB tags" below).
  `smp::quiesce_root(root)` sends every other CPU whose window uses `root`
  an IPI and waits until that window closed; backends call it before a
  translation changes or its tables are freed. The waiting CPU holds the
  kernel lock, which the kicked CPUs need only after they acknowledged.
- **Placement.** With one CPU online the runtime keeps its `(pid, tid)` round
  robin. With secondaries online, every runnable thread that is not on a CPU
  goes to the run queue of its CPU (`BasicScheduler::enqueue_thread`: its last
  CPU, or the shortest allowed online queue for a thread that never ran);
  each CPU takes the next entry from its own queue or steals one, dispatches it
  with `mark_on_cpu` and wakes an idle CPU it handed work with a reschedule
  IPI. Native processes may run on every online CPU.
- **Per-CPU records** (`ok/smp/bringup.hpp`, header-inline): state, the
  started and reported hardware ids, stack bounds, the user window and
  counters. With more than one CPU online the interrupt report ends with
  `OK_SMP_CPUS scope=hardware online=N cpuK=<user entries>:<local ticks>:<IPIs
  received>:<IPIs sent>:<shootdowns waited for>`.

## Profiles

| Profiles | Discovery | Start | IPI | Local timer |
| --- | --- | --- | --- | --- |
| x86_64 | ACPI MADT (RSDP in the EBDA or BIOS area) | INIT-SIPI-SIPI through the xAPIC | fixed xAPIC IPI | LAPIC timer |
| aarch64 | FDT `/cpus`, `/psci` (0.2+, `hvc` or `smc`) | PSCI `CPU_ON` | GICv2 SGI | banked physical timer, PPI 30 |
| rv64 | FDT `/cpus` | harts parked in `boot.S`, released through the CLINT MSIP | CLINT MSIP | per-hart `mtimecmp` |
| i386, arm32, rv32, loongarch64, mips, mips64, ppc | none | none | none | none |

QEMU Malta has no power controller for its cores and sends every CPU from its
bootloader to the kernel entry, as `-bios none` does on RISC-V: mips and
mips64 park every CPU but 0 (EBase.CPUNum) in `boot.S`, and rv64 and rv32 park
every hart but 0.

rv64 and rv32 run the kernel in machine mode with `-bios none`, so no SBI
firmware exists to call: the plan's "SBI HSM" is replaced by an HSM-shaped
start inside the kernel, reported as `ap_start=clint-msip`. rv32 parks its
secondary harts for good. The other UP profiles link `src/arch/smp_stub.cpp`
and print `OK_SMP scope=hardware cpus=1 online=1 ap_start=unsupported
ipi=unsupported reason=profile-up`.

## Slice 1 results

- Hosted model suites (scope=model): `scripts/test_smp_locks.py`,
  `scripts/test_run_queues.py` (525 checks on each of the ten profiles),
  `scripts/test_cpu_topology.py` (synthetic FDT and ACPI cases, a mutation
  corpus, and four DTBs dumped by QEMU 10.2.0 virt with `--qemu-dir`), and
  `scripts/test_kernel_entry_stack_pool.py` (one active lease per CPU).
- `tests/qemu/test_qemu_runner.py`: 40 tests, including the `-smp` command and
  marker validation.
- `xmake test` passes on all ten profiles with one CPU (the UP line above),
  and `scripts/hosted_suite.py --toolchain-root` runs 145 runners, all passing
  (58 need fixtures or inputs, or are quarantined), including every
  per-architecture execution probe.
- The paired ObfuscationOS distro (`wt/os-smp-bringup`) boots natively on
  x86_64 against this SDK with one CPU (`qemu-distro --fs=simplefs
  --require-native`: `OK_USERLAND scope=hardware acceptance=pass`).

## Slice 2 results (QEMU 10.2.0, debug kernels)

- `xmake test --qemu_smp=N`:
  - x86_64 `OK_SMP scope=hardware cpus=N online=N ap_start=sipi ipi=pass`,
    aarch64 `ap_start=psci-hvc`, rv64 `ap_start=clint-msip`, for N = 2 and 4.
  - aarch64 with `-M virt,virtualization=on` (entry at EL2), N = 2:
    `ap_start=psci-smc ipi=pass`.
  - i386, arm32, rv32, loongarch64, mips, mips64 and ppc with N = 2:
    `cpus=1 online=1 ap_start=unsupported ipi=unsupported reason=profile-up`.
- Native execution probes: `scripts/test_native_x86_64.py --smp 2`
  (`OK_NATIVE_SMP scope=hardware cpus=2 secondary_user=pass syscall=pass
  local_timer=pass ipi=pass shootdown=pass boot_pit=pass`),
  `scripts/test_native_rv64.py` on two harts (`RV64_NATIVE harts=2
  start=pass local_timer=pass shootdown=pass idle_ipi=pass`),
  `scripts/test_native_aarch64.py`, `scripts/test_native_arm32.py` and
  `scripts/test_native_rv32.py`.

## Slices 3 and 4 results: native threads on several CPUs

The paired ObfuscationOS worktree `feat/smp-bringup` (`wt/os-smp-bringup`)
consumes this tree's debug SDK. `scripts/qemu_distro.py` takes `--smp N`.

- Distro boot (`--fs=simplefs`, native init, `-smp N`): `OK_USERLAND
  scope=hardware acceptance=pass shell_pipeline=pass` on x86_64, aarch64 and
  rv64 with N = 2 and 4, every online CPU with user entries in `OK_SMP_CPUS`
  (for example x86_64 N = 4: `cpu0=859 cpu1=1078 cpu2=587 cpu3=576`).
- Exit gate: `scripts/test_smp_stress.py` boots the scratch ext4 root and runs
  the static musl probe `tests/musl/smp_stress.c` (6 workers x 40 rounds: files
  in private and one shared directory with fsync/rename/readback, AF_UNIX
  socketpair ping-pong with a forked peer, fork plus exec or exit, and a
  thread that keeps writing its own pages while the worker maps,
  write-protects and unmaps others, so the address space changes under a
  thread running on another CPU). It requires `OK_SMP scope=hardware cpus=N
  online=N`, `SMP_STRESS_PASS` with the exact counts, and user entries during
  the probe on every online CPU. Three runs per configuration, all pass
  (kernel at c0f82db):

| Profile | vCPUs | Runs | Guest seconds | User entries per CPU (first run) | Shootdowns waited per CPU (first run) |
| --- | --- | --- | --- | --- | --- |
| x86_64 | 2 | 3/3 | 35.6, 36.3, 36.7 | 22880, 23535 | 481, 832 |
| x86_64 | 4 | 3/3 | 33.7, 33.8, 34.0 | 12222, 13885, 12441, 13969 | 224, 426, 415, 561 |
| aarch64 | 2 | 3/3 | 50.9, 47.4, 45.8 | 23273, 19103 | 726, 413 |
| aarch64 | 4 | 3/3 | 41.5, 41.4, 40.3 | 10786, 9593, 10600, 10653 | 292, 447, 529, 542 |
| rv64 | 2 | 3/3 | 39.0, 43.3, 40.7 | 20858, 24109 | 164, 843 |
| rv64 | 4 | 3/3 | 34.2, 33.6, 34.1 | 10123, 10385, 10803, 11592 | 222, 458, 603, 619 |

  Every run reports `files=480 messages=960 forks=480`. The same probe passes on
  one vCPU (x86_64, 42.6 s).
- Defects found by the gate and fixed before these runs: a driver waiting
  for its device inside a syscall on a secondary halted holding the kernel
  lock for an interrupt only CPU 0 takes (`wait_for_interrupt()` now returns
  false off CPU 0, so the driver polls there), and the model `Kernel::tick()`
  replaced the current thread of a secondary in its user window (it now
  schedules only the calling CPU).

## Pre-merge review

An adversarial review (one reviewer each for x86_64, aarch64, rv64 with the
generic layer, and the runtime; one skeptic per finding) confirmed four
findings; three were fixed before the merge and the exec one in the
follow-up:

- A sibling's `mmap`, `munmap` or `mprotect` replaces the address space while
  another thread of the process runs on another CPU; the capture failed that
  entry and killed the process. It now takes the live space after the entry.
  The stress probe's address-space phase covers it.
- The shared monotonic clock jumped centuries forward when the CPU holding the
  kernel lock read a TSC behind the previous reader's, and later invalidated
  itself; a backward 64-bit step now stalls it.
- Shutdown reservation and native termination refused while any other CPU
  ran user code; they now check the calling CPU's own entry.
- Exec evicted siblings before its fallible steps; every step that can
  refuse an exec now precedes the eviction ("Exec ordering" below).

A secondary the boot CPU gave up on (late or with a mismatched id) now stays
out of the online state; the review rated that hardening.

## Lifting the bring-up limits

Seven follow-up items on the same branch (`git log 023386b..b28815c`), each
with its own model tests and QEMU evidence. Unless noted, guest evidence is
QEMU 10.2.0 with debug kernels; the OS gates run in the paired worktree
`wt/os-smp-bringup`.

### Exec ordering

Exec now makes every step that can refuse it while siblings still run on
other CPUs: `SchedulerState::preflight_replace_image`, the entry stack plan,
`prepare_eviction` (which validates each planned CPU's active lease), the
check that every other leased thread is inactive, and the VFS retirement
prepare. The futex, UDP, syscall-origin and VFS-continuation preflights that
follow predict invariant breaches; they, and any failure after
`commit_eviction`, stop the runtime without resuming the caller. An exec that
evicted entries prints `OK_NATIVE_EXEC_EVICT pid= tid= evicted=N`.

- `scripts/test_native_exec_eviction.py`: exec with a sibling on a modelled
  second CPU through the actual runtime, loader, entry stack pool and
  scheduler, on all ten profiles (scope=model; mock trap, copy and mapping
  backends).
- No guest transcript yet shows `OK_NATIVE_EXEC_EVICT`.

### Atomic futex user words

Futex words are updated through the kernel pointer to the word's backing
frame (`arch::execution::user_word`, `src/user/futex_word.hpp`):
FUTEX_WAKE_OP's second word is one atomic read-modify-write after
copy-on-write, FUTEX_WAIT, WAIT_BITSET and CMP_REQUEUE compare with one atomic
load, robust-list retirement uses an atomic load and compare-exchange, and
`clear_child_tid` is one atomic store. The futex queues stay under the kernel
lock. i386 (no CMPXCHG, UP) keeps plain aligned accesses; a profile that
starts secondaries refuses to build without the hardware operation. The SDK
profiles become
`kernel-lock-atomic-word-two-key-private-anonymous-shared-signed-wrap-legacy-count-v2`
and
`kernel-lock-atomic-compare-nonpi-read-keys-signed-counts-exact-origin-ticket-sequence-owned-transfer-v2`,
with `smp_atomic = 1`.

- `scripts/test_futex_word.py` races the primitives against host threads
  (scope=model); `scripts/test_native_futex_operations.py` runs the wake-op
  and requeue suites.
- OS `scripts/test_futex_smp_atomic.py`: the static musl probe races
  FUTEX_WAKE_OP updates of one word against user atomics on another CPU, then
  checks a copy-on-write word after fork. Integrated tree, 20000 rounds:

| Profile | vCPUs | Result | `interleaved` |
| --- | --- | --- | --- |
| x86_64 | 2 | `FUTEX_SMP_ATOMIC_PASS` | 19975 |
| x86_64 | 4 | `FUTEX_SMP_ATOMIC_PASS` | 19946 |
| aarch64 | 2 | `FUTEX_SMP_ATOMIC_PASS` | 19998 |
| aarch64 | 4 | `FUTEX_SMP_ATOMIC_PASS` | 19949 |
| rv64 | 2 | <!-- FINAL: rv64 smp=2 futex gate result --> | <!-- FINAL --> |
| rv64 | 4 | <!-- FINAL: rv64 smp=4 futex gate result --> | <!-- FINAL --> |

<!-- FINAL: replace `interleaved` with the honest overlap evidence the futex probe will report, and add the negative control (the probe losing updates on 023386b at 2 vCPUs) with its receipt path. -->

### x86 TSC synchronization

Each started AP runs a TSC handshake with the boot CPU before it enters the
kernel (64-round batches, up to 16, until the bound is within 50 us); a
measured offset is corrected through `IA32_TSC_ADJUST` or a per-CPU software
offset and verified by measuring again. An AP that cannot be verified parks
before it reads the clock and stays offline. `OK_SMP` gains
`tsc=sync|adjusted|offset`, and each AP prints `OK_TSC_SYNC`. The SMP
profiles' monotonic clock samples without a lock above a shared floor.

- `scripts/test_tsc_sync.py`: the estimator and the correction decision,
  including the `IA32_TSC_ADJUST` path, against injected skew and noise
  (scope=model).
- `xmake test --qemu_smp=2` and `4` on x86_64: `tsc=sync`, every AP within
  its bound; at 4 vCPUs `offset=133 error=602`, `offset=128 error=660` and
  `offset=193 error=904` cycles (slack 369).
- `x86_tsc_skew_test` at 2 vCPUs: CPU 1's rewind went into the software
  offset (`skew_test=software`), was measured as `offset=-23576246908
  error=400` and corrected to `result=offset residual=-101
  residual_error=407 tolerance=769`; `qemu_test.py --tsc-skew-test` accepts
  it. <!-- FINAL: receipt path of the skew-knob run. -->

### User TLB tags

User entries run under per-CPU tags (aarch64 ASIDs, rv64 satp ASIDs, x86_64
PCIDs) from one 32-slot cache per CPU, revalidated against each space's TLB
generation; exit no longer invalidates, and only untagged CPUs flush the user
TLB on every entry. Bring-up prints `OK_SMP_TLB`.

- `scripts/test_tlb_tags.py`: the generation protocol, rollover and stale
  tags (scope=model).
- Lanes at 2 and 4 vCPUs: x86_64 `asid=none pcid=off` (TCG offers no PCID),
  aarch64 `asid=16`, rv64 `asid=16`.
- Native probes: aarch64 `OK_NATIVE_TLB asid=16 churn=pass rollover=pass
  remap=pass`, rv64 `RV64_NATIVE tlb_asid=16 churn=pass rollover=pass
  remap=pass`, x86_64 `OK_NATIVE_TLB asid=none pcid=off churn=pass
  rollover=untagged remap=pass`; with two CPUs `remote_remap=pass` on all
  three (a secondary re-enters a space after a remap shootdown and sees the
  new frame).

### Device interrupt routing

Device lines are routed to any online CPU on aarch64 (GICv2), rv64 (PLIC) and
x86_64 (the I/O APIC, which replaces the 8259 there), spread as line mod N,
and follow a device waiter on another CPU. Secondary CPUs run top halves in
their user trap path, idle wait and kernel-lock spin; a driver waiting inside
a syscall on a secondary halts there. `retarget_platform_irqs_away()` moves a
CPU's lines away for hotplug. The interrupt report adds `OK_SMP_IRQS`
(`docs/modules/interrupts.md`).

- `scripts/test_irq_wait_time.py`: the router, the wake protocol and the
  retarget/follow race (scope=model).
- Native probes: x86_64 `OK_NATIVE_IOAPIC boot_wait=pass pit_gsi=pass
  uart_gsi=pass`, `cpus=2 user_top_half=pass wake_ipi=pass idle_top_half=pass
  secondary_device_wait=pass spin_serve=pass` and `retarget_away=pass`;
  aarch64 `OK_NATIVE_GIC cpus=2 user_top_half=pass wake_ipi=pass
  idle_top_half=pass kernel_lock_wait=pass spin_serve=pass
  retarget_away=pass`; rv64 `RV64_NATIVE device_route=hart1
  user_top_half=pass wake_ipi=pass`, `idle_top_half=pass
  secondary_device_wait=pass spin_serve=pass` and `retarget_away=pass`.
- OS `scripts/qemu_distro.py --smp N` requires the bound lines spread over at
  least two online CPUs on native SMP runs. Stress-gate examples at 4 vCPUs:
  x86_64 `routes=1@1,4@0,11@3,12@0`, aarch64
  `routes=33@1,76@2,77@1,78@2,79@3`.
  <!-- FINAL: qemu_distro acceptance runs (x86_64 at 1/2/4, aarch64 and rv64 at 2/4) on the integrated SDK and their receipt paths. -->

### Kernel sections

The kernel lock is taken only through the BKL helpers, which keep per-CPU
mode words and accounting; shared clock, console and address-space state moved
under ranked locks (`ofd_position` 14, `fs_provider` 16, `arch_spaces` 64,
`realtime_clock` 82, `monotonic_clock` 84, `console` 90), and a successful
`try_lock` is order checked. Debug kernels check the kernel-lock contracts
(`exclusive_asserts`, `drain_asserts`). Linux `getpid`, `gettid`,
`clock_gettime`, `clock_getres` and ext4 regular-file `read`, `write`,
`pread64`, `pwrite64`, `lseek`, `fsync` and `fdatasync` run as kernel
sections without the kernel lock, under a description position lock and the
ext4 operation gate, with their results committed under the lock.
`docs/modules/smp.md` describes the protocol, admission, drains and the
evidence lines.

- Model tests: `scripts/test_smp_locks.py`,
  `scripts/test_kernel_section_protocol.py`,
  `scripts/test_ext4_gate_concurrency.py` (`--tsan` adds ThreadSanitizer),
  `scripts/test_linux_file_io_equivalence.py` (ten profiles; each decline
  checked against its digest), `scripts/test_realtime_clock.py` and
  `scripts/test_clock_counter.py`.
- OS `scripts/test_smp_stress.py` (default `--smp-sections on`) requires the
  families to cover the probe's file traffic (`fsync >= files`; `read`,
  `write`, `lseek >= 2 * files`); with two or more CPUs, sections on at least
  two CPUs, `concurrent` and `witnessed` above 0 and `max_parallel >= 2`; on a
  debug kernel every assertion counter and shadow mismatch at 0. The receipt
  records QEMU's TCG thread mode from its thread names; every run below was
  multi-threaded TCG. Integrated tree, one run each, all `SMP_STRESS_PASS
  files=480 messages=960 forks=480`, `sections=completed=3362` (`read=960
  write=960 lseek=960 fsync=480 clock_gettime=2`), `shadow=1/0` and every
  assertion counter 0:

| Profile | vCPUs | Guest s | concurrent | witnessed | max_parallel | handoffs | spun | drain_waits | bkl_acquired | bkl_contended |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| x86_64 | 2 | 51.1 | 942 | 1645 | 2 | 106 | 203 | 43 | 93329 | 57993 |
| x86_64 | 4 | 55.6 | 1530 | 2007 | 3 | 190 | 436 | 51 | 90218 | 80025 |
| aarch64 | 2 | 45.2 | 962 | 1280 | 2 | 80 | 173 | 41 | 91569 | 53395 |
| aarch64 | 4 | 37.7 | 1673 | 1734 | 3 | 276 | 413 | 102 | 86642 | 81350 |
| rv64 | 2 | <!-- FINAL --> | | | | | | | | |
| rv64 | 4 | <!-- FINAL --> | | | | | | | | |

Guest seconds come from a host shared with other jobs and are not a speed-up
measurement.

<!-- FINAL: the smp_sections=n arm (no OK_SMP_KERNEL line), one release run per profile, the 1-vCPU runs, a guest-seconds comparison of both arms on a quiet host, and the receipt paths. -->

### CPU hotplug

`smp::stop_secondary_cpu` and `restart_secondary_cpu` take a started
secondary offline and back: the scheduler moves its queue, its section is
drained, it leaves the online mask before its device lines move, and it parks
(x86 `cli; hlt` until INIT, aarch64 PSCI CPU_OFF confirmed by AFFINITY_INFO,
rv64 the `boot.S` park loop). Restart reuses its stack and start path and
requires an IPI answer.

- Every debug SMP boot runs the test point: `OK_SMP_HOTPLUG_BOOT
  scope=hardware cpu=1 stop=pass parked_ipi=pass restart=pass ipi=pass` in
  all six integrated lanes (x86_64, aarch64, rv64 at 2 and 4 vCPUs) and in
  every stress-gate boot above; on the hotplug branch (0bf4c90) 18 of 18
  lanes, three runs each.
- OS `scripts/test_smp_stress.py --require-hotplug` against a
  `smp_hotplug_test=y` kernel: the last `OK_SMP_HOTPLUG` line must show
  `cycles > 0`, `failures=0` and a secondary CPU, with counters that never go
  back. On the hotplug branch (0bf4c90), two runs per configuration, all
  `SMP_STRESS_PASS` with `failures=0`:

| Profile | 2 vCPUs (cpu=1): cycles | 4 vCPUs (cpu=3): cycles |
| --- | --- | --- |
| x86_64 | 782, 582 | 865, 502 |
| aarch64 | 725, 768 | 684, 735 |
| rv64 | 725, 723 | 697, 675 |

<!-- FINAL: the integrated hotplug gate (x86_64, aarch64, rv64 at 2 and 4 vCPUs) with the OS gate's checks for hotplug cycles during the probe and the release assertion counters, and the receipt paths. -->

### Integrated validation

<!-- FINAL: kernel sha of the integrated run (b28815c or the final merge) and the paired OS sha. -->

- `xmake test` passes on all ten profiles with one CPU, and on x86_64,
  aarch64 and rv64 with `--qemu_smp=2` and `4`: `OK_QEMU_SMP_TEST ...
  ipi=pass tsc=sync tlb_asid=none pcid=off hotplug_boot=pass` (x86_64),
  `tlb_asid=16 pcid=off hotplug_boot=pass` (aarch64, rv64).
- `scripts/hosted_suite.py --toolchain-root`: 154 passed, 0 failed, 58 not
  run (fixtures, inputs or quarantine).
- Native probes pass: x86_64 with one and two CPUs, aarch64 with two, rv64
  with two harts, arm32, rv32 and i386.
- `tests/qemu/test_qemu_runner.py`: 39 tests.

<!-- FINAL: OS gate totals, the receipt directory in ObfuscationEvidence and the module ABI number. -->

## Limits

- Kernel work outside kernel sections is serialized by one lock. Only the
  Linux identity and clock calls and ext4 regular-file I/O run as sections;
  thread selection and entry, every blocking call, the VFS namespace, pipes,
  sockets, PTYs, fork/exec/exit/wait, signals, timers, futex queues, memory
  mapping and copy-on-write faults, networking, module load, power, device
  bottom halves and interrupt binding stay under the lock
  (`docs/modules/smp.md`, "What stays serialized").
- `O_APPEND` writes and nodes other than ext4 stay under the lock. ext4 runs
  one operation at a time per filesystem, so file sections never do disk work
  in parallel; each description serializes its own offset I/O; a process's
  other syscalls, every reap and every `O_APPEND` change wait for sections.
- A driver waiting for its device under the kernel lock keeps the lock while
  it halts; inside a section it polls.
  <!-- FINAL: state what the pending fix changes for a CPU polling a device inside a kernel section (serving its own interrupt controller). -->
- The futex queues stay under the kernel lock; file-backed keys,
  cross-process `CLONE_VM`, PI, realtime priority and lock-free queues remain
  gaps.
- x86_64 PCIDs are implemented but not exercised: QEMU TCG offers no PCID and
  KVM was not available, so x86_64 runs untagged. TCG drops its TLB on ASID
  and satp changes, so QEMU runs exercise the tagged paths and encodings but
  cannot detect a missing invalidation; only the hosted model checks the
  generation protocol.
- The `IA32_TSC_ADJUST` correction path has model coverage only: under TCG
  every vCPU reads one counter, and the skew knob's rewind goes through the
  software offset.
- Hotplug has no user-facing interface; only the debug test point and the
  `smp_hotplug_test` knob request it. Device lines are not rebalanced onto a
  restarted CPU. A stop holds the kernel lock during its bounded wait for the
  user window, and an x86 restart holds it across INIT and the TSC handshake.
  No stop timed out in any run, so the timeout path is unexercised.
- No guest run shows an exec evicting a sibling on another CPU
  (`OK_NATIVE_EXEC_EVICT`); the exec ordering has model evidence only.
- The global tick, `ok_interrupt_tick()`, the timer latency histogram and the
  device event sources stay on CPU 0. An x86_64 boot left on the 8259
  fallback polls device waits on secondaries. i386, arm32, rv32, loongarch64,
  mips, mips64 and ppc run UP.
- Every profile runs on QEMU TCG only; nothing here is evidence for physical
  CPUs or physical hotplug.
