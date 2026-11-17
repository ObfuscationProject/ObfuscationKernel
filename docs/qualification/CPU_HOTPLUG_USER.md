# User CPU hotplug (smp-scaling WP1)

Userland can now take secondary CPUs offline and back with Linux's
interface. Root writes `0` or `1` to `/sys/devices/system/cpu/cpuN/online`,
and threads use `sched_getaffinity`, `sched_setaffinity` and `getcpu`. The
design is in `docs/modules/sysfs.md`.

Everything runs under the big kernel lock (BKL): the write route, the service
on CPU 0, completion and wakeup. Baseline: a10aeb9 (WP0a on main 5815814,
module ABI 81). The ABI number is not bumped here, and no SDK carries a
development tag (the lead waived it for this series).

## What it adds

- **Kernel**
  - `fs::CpuSysFileSystem` (`ok/fs/sysfs.hpp`), the `Node::control_node()`
    hook with `fs::ControlNode` (`ok/fs/control_node.hpp`), and
    `OpenFileDescription::control`. Open follows kernfs rules.
  - The write route (`src/user/runtime/hotplug.cpp`) for `write`, `writev`
    and `pwrite64`, with `ThreadWaitReason::cpu_hotplug` (16).
  - `smp::hotplug::HotplugService` (`ok/smp/hotplug.hpp`) and its kernel
    backend and CPU 0 pass (`src/smp/hotplug.cpp`).
  - Per-thread affinity masks with the broken-affinity reset, the
    `offline_cpu` precheck, and the kernel and preemption ticks limited to
    kernel threads once userland runs.
  - `sched_getaffinity`, `sched_setaffinity` and `getcpu` on all ten Linux
    profiles (generator and data regenerated from musl 1.2.5 and Linux
    6.13.4) and in the native table.
  - The `OK_SMP_HOTPLUG_USER` and `OK_SMP_AFFINITY_BROKEN` lines.
- **ObfuscationOS** (paired change, delivered as a patch on `wt/os-smp-scaling`
  46b763d)
  - `/sys` is mounted after `/dev` (`modules/boot/storage.cpp`; the
    mount-point helper is generalised).
  - `apps/chcpu` (`-e`, `-d`, `-l`) is staged on every profile. The SimpleFS
    image omits it, like the other service programs, because of its
    32-entry cap.
  - `tests/musl/cpu_sysfs_probe.c` and `scripts/test_cpu_sysfs.py`.
  - The `smp-stress W R hotplug` controller and
    `scripts/test_smp_stress.py --user-hotplug`.

## Deviations from the specification

- **Under the BKL, not `task`.** Section 6.4's CS2 checks the writer under
  the BKL with `find_thread`. `ProcessPin` and the `task` lock arrive in WP2
  and WP4a. For the same reason the scheduler's `online_cpus_` stays a plain
  `u32`: every reader and writer holds the BKL. It becomes atomic when
  `offline_cpu` leaves the BKL (WP7b).
- **Thread masks live in the TCB.** The mask is
  `ThreadControlBlock::cpu_affinity_mask` (u16, like the process mask), not
  `ProcessSlot::thread_affinity`, which WP2 introduces.
- **Where the hooks sit.**
  - The broken-affinity reset is `SchedulerState::repair_affinity`. It is
    called by `select_cpu` (so by `enqueue_thread` and the `offline_cpu`
    drain) and by the step's scan before it picks a thread directly. Section
    4.2's `enqueue_locked` does not exist yet.
  - `select_cpu` keeps a CPU 0 fallback, but only for a *process* mask that
    names no configured CPU. `affinity_admits` admits that case today.
- **The tick restriction is in one place.** It sits in
  `BasicScheduler::schedule_next_on_cpu`, which both `Kernel::tick` and
  `PreemptionController::tick` call, through `schedule_kernel_thread_on_cpu`.
  `process_allowed_on` is unchanged.
- **No killable flag.** A waiting writer dies through the existing
  step-level fatal pass (`private_process_signal_fatal` → `finish`), whose
  thread retirement drops its record (`cpu_control_retire` in
  `retire_native_thread`). WP4d replaces that pass with `pending_exit`.
- **Resume.** A woken writer's result is returned by the step's resume chain
  (a lock-free per-record hint, then `consume`), before any frame decoding.
  It is also returned by the route itself. A result whose descriptor went away
  is dropped by the slot's next request or at thread teardown.
- **A slot still busy.** A slot whose last owner died while its request ran
  answers a new request with EBUSY until the service frees it.
- **`chcpu -l` is local.** util-linux lists CPUs with `lscpu -e`. `chcpu`
  prints each message with one write, so kernel lines never split it.
- **Mode 0555 on `cpuN/`.** The spec's 0555 is kept for `cpuN/` (Linux uses
  0755). The other directories are 0755.
- **Changed model tests.** `tests/host/run_queue_tests.cpp` `test_hotplug` and
  `test_stopped_processes` pinned threads with *process* masks and expected
  `offline_cpu` to park them on CPU 0. The precheck now refuses that. The tests
  pin with thread masks instead. They check the new outcome (broken affinity,
  the ring, nothing waiting for the offline CPU) and the precheck refusal
  itself.
- **The killed-writer check.** With two vCPUs the controller cannot run
  while CPU 0 stops the other CPU. It can only kill a writer around the time
  it files, or one holding an unconsumed result, and the result races the
  writer's re-dispatch. The gate therefore requires at least one landed kill
  everywhere. It requires at least one kill after the request was filed (the
  CPU went offline although its writer died) only with three or more vCPUs.
- **Release musl.** The shared release musl trees fail the probe builder's
  artifact check (`Scrt1.o` mismatch). Release runs used private builds of
  the pinned archive.

## Validation

All guest runs are QEMU TCG on the pinned SysToolchain QEMU 10.2
(`scope=hardware` means the emulated machine). Model tests print
`scope=model`. The host was shared and heavily loaded (load average about
40 on 24 cores) during these runs, so guest timings, including `max_us`,
include host scheduling delays.

### Hosted (model scope)

`scripts/hosted_suite.py --toolchain-root ... --jobs 8` reported 165 passed,
1 failed and 59 not run (inputs, quarantine, or no ext4 fixture). The failure
was `test_native_pty.py`: its explicit runtime source list lacked the new
`src/smp/hotplug.cpp` and its dependencies. The fix is in this package, and
the rerun passed. The suite includes the new runners:

- `test_sysfs_cpu.py`;
- `test_cpu_hotplug_service.py`: OK_HOSTED_LOCK_MODEL with and without
  OK_LOCK_DEBUG, plus the UpLock build;
- `test_run_queues.py`: ten profiles, with `test_thread_affinity` and
  `test_kernel_tick`.

It also includes `test_linux_bootstrap.py` (the regenerated tables on ten
profiles), `test_static_dispatch.py` and `test_smp_locks.py`.
`scripts/generate_linux_abi.py --check` matches the committed data.

### Kernel

`okernel_image` (debug) builds on all ten profiles. `xmake test` passes at
`-smp 1` on all ten profiles, and at `-smp 2` and `-smp 4` on x86_64, aarch64
and rv64. The debug boot test point stops and restarts CPU 1 on the SMP
profiles.

### ObfuscationOS

These runs used a private copy of `wt/os-smp-scaling` 46b763d with the paired
patch.

- **Native distro boot** to `OK_USERLAND scope=hardware acceptance=pass` on
  all ten profiles (`distro10.sh`). Every boot mounted `/sys`. Every check
  counter was 0, with `max_lock_depth=3`.
- **CPU sysfs probe** (`test_cpu_sysfs.py`): `CPU_SYSFS_PASS` on all ten
  profiles at one vCPU, and at 2 and 4 vCPUs on x86_64, aarch64 and rv64.
  - It checked the tree, `nproc --all`, sched_getaffinity (8 bytes on 64-bit
    profiles, 4 on 32-bit), sysconf, and getcpu after pinning to every online
    CPU.
  - The refusals: 6 on one CPU (UP profiles show the files and refuse every
    write open) and 9 with secondaries, including uid 1000.
  - On SMP boots `OK_SMP_HOTPLUG_USER` showed `requests=2 noop=2` and nothing
    else.
- **`test_smp_stress.py --user-hotplug`**, debug, at 2 and 4 vCPUs on
  x86_64, aarch64 and rv64, and in release at 2 vCPUs on each. Zero failures:
  every run had `failures=0 wedged=0`, online equal to offline, and an
  `OK_SMP_AFFINITY_BROKEN` line for every announced pinned child. Debug runs:

  | Profile | vCPUs | cycles | pinned | racing | killed (filed) | offline/online | max_us |
  |---|---|---|---|---|---|---|---|
  | x86_64 | 2 | 32 | 8 | 8 | 3 (2) | 39/39 | 932421 |
  | x86_64 | 4 | 15 | 11 | 11 | 3 (2) | 46/46 | 82524 |
  | aarch64 | 2 | 35 | 9 | 9 | 2 (1) | 37/37 | 10862 |
  | aarch64 | 4 | 10 | 8 | 7 | 6 (3) | 32/32 | 18805 |
  | rv64 | 2 | 34 | 9 | 8 | 2 (1) | 39/39 | 11638 |
  | rv64 | 4 | 14 | 11 | 10 | 8 (6) | 43/43 | 45899 |

  The 0.93 s stop on x86_64 at 2 vCPUs is below the 2 s stop timeout. It
  happened under the host load noted above; the other runs stayed under
  0.1 s. Each run also exercised `/bin/chcpu -d` and `-e` and the five
  refusals (cpu0/online ENOENT and EACCES, `online` EACCES, "x" EINVAL, uid
  1000 EACCES).
- **Standard gates.** All passed with every `OK_SMP_KERNEL_CHECKS` and
  `OK_SMP_CHECKS` counter and `lock_violations` at 0, and `max_lock_depth`
  4 in debug:
  - `test_smp_stress.py` debug at 2 and 4 vCPUs and release at 2 vCPUs on
    the three SMP profiles;
  - the futex probe at 2 vCPUs on each;
  - `--smp_hotplug_test=y --require-hotplug` at 2 vCPUs on each: 515, 783
    and 555 cycles, `failures=0`;
  - `smp_sections=n` at 2 vCPUs on each.
- **Not run.** No BusyBox was in the staged rootfs, so the BusyBox bonus
  check (`echo 0 > .../cpu1/online`) was not run.
