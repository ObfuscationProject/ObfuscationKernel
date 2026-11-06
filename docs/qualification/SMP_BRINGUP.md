# SMP bring-up

WT2.1 `feat/smp-bringup`. Slice 1: CPU primitives, ranked locks, per-CPU run
queues, CPU discovery parsers and the QEMU `-smp` lane. Slice 2: secondary CPU
start, IPIs, local timers and per-CPU execution state on x86_64, aarch64 and
rv64. Slice 3: the native runtime serves threads on every online CPU. Slice 4:
the concurrent filesystem/socket/fork stress gate. This note grows with each
slice.

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
  to the kernel root, which discards the user TLB on every profile.
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
  socketpair ping-pong with a forked peer, fork plus exec or exit). It requires
  `OK_SMP scope=hardware cpus=N online=N`, `SMP_STRESS_PASS` with the exact
  counts, and user entries during the probe on every online CPU. Three runs
  per configuration, all pass:

| Profile | vCPUs | Runs | Guest seconds | User entries per CPU (first run) |
| --- | --- | --- | --- | --- |
| x86_64 | 2 | 3/3 | 49.4, 54.4, 49.9 | 17494, 17222 |
| x86_64 | 4 | 3/3 | 72.6, 92.8, 92.4 | 10434, 12272, 11328, 12751 |
| aarch64 | 2 | 3/3 | 50.6, 42.5, 34.0 | 15077, 14541 |
| aarch64 | 4 | 3/3 | 39.8, 37.3, 37.0 | 7304, 7074, 7857, 6799 |
| rv64 | 2 | 3/3 | 30.5, 31.7, 30.2 | 15644, 13950 |
| rv64 | 4 | 3/3 | 33.1, 32.2, 30.2 | 7466, 7272, 7427, 7702 |

  Every run reports `files=480 messages=960 forks=480`. The same probe passes on
  one vCPU (x86_64, 42.6 s).
- Two defects found by the gate and fixed before these runs: a driver waiting
  for its device inside a syscall on a secondary halted holding the kernel
  lock for an interrupt only CPU 0 takes (`wait_for_interrupt()` now returns
  false off CPU 0, so the driver polls there), and the model `Kernel::tick()`
  replaced the current thread of a secondary in its user window (it now
  schedules only the calling CPU).

## Limits

- Kernel work is serialized by one lock; only user code runs in parallel.
- The futex wake-op and requeue paths update user words with checked copies
  under the kernel lock, which is not atomic against user code on other CPUs
  (`native_futex_*_smp_atomic = 0` in the SDK profile).
- No CPU hotplug, no ASIDs or PCIDs (every entry flushes the user TLB), and
  device interrupts are not balanced across CPUs: they stay on CPU 0, and a
  device wait on a secondary CPU polls.
- The stress gate runs single-threaded processes, so its runs report no
  shootdowns; shootdown is exercised by the x86_64 and rv64 execution probes.
  The aarch64 probe still runs on one CPU.
- Every profile runs on QEMU TCG only; nothing here is evidence for physical
  CPUs.
