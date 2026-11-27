# SMP scaling WP4d: signals, group transitions, job control

Signal and lifetime package of the smp-scaling work (spec sections 4.7, 4.8,
4.9 and 4.12). Exec and exit_group no longer take another CPU's user entry
over: they freeze their thread group and wait, with the big kernel lock
(BKL) released, until every other thread of the group left its CPU through
its own `put_prev`. A kill never waits for another process: it sets a
pending exit that a thread of the target applies at its next pick. Signal
generation probes the waits it may interrupt, so the per-step signal scan
is gone. The step still runs under the BKL. Baseline: d7fde31 (WP4b after
its review). The module ABI is not bumped.

## What it adds

- **Group transitions** (`src/sched/group_transition.inc`,
  `include/ok/sched/group_quiesce.hpp`). A process slot gains its
  transition claim (`none`, `exec` or `exit`, and the claiming tid) and the
  status of a pending exit, both under the slot's `task` lock.
  `claim_transition` freezes the group (`RunFlag::frozen`), drops every other
  thread's queue entry and returns the CPUs that still run one. Outcomes:
  claimed; held (the same claimer); `exit_held` (another thread exits the
  group: return to the boundary); `exec_held` (a second exec: the winner's
  commit exits this thread, Linux `de_thread`); `exit_pending` (an exit
  wanted while an exec runs sets `pending_exit`: exit beats exec).
- **quiesce_group** (`group_quiesce.hpp`), a template over a `QuiesceHost`
  concept (kernel lock, interrupt service, clock): claim, kick the CPUs that
  run a sibling (`ipi_kick`), and when a sibling is still on a CPU release
  the BKL (rule W0) and spin, serving interrupts, until every sibling's
  `on_cpu` mirror reads `no_cpu` (an acquire load paired with `put_prev`'s
  release store). Each 2 s without progress counts one `quiesce_timeouts`
  and kicks again; the wait continues. The BKL is taken again before it
  returns. `check_class_wait` asserts that no spin lock is held.
- **The frozen check** (`smp::bkl_frozen_check`, installed when the runtime
  starts: `group_frozen`). After its user entry returned and its capture was
  checked, the step asks it under the post-user relock. A frozen thread skips
  trap handling, sections and dispatch: a trapped syscall stays pending (the
  frame keeps it), a timer tick is counted, a fault is taken again later, and
  `put_prev` takes the thread off its CPU without queueing it. A kept
  syscall carries `syscall_deferred` and its MIPS branch-delay-slot exclusion
  (`syscall_delay_slot`) in its TCB: after a thaw it is dispatched as the
  trap it was (never with a stored hotplug result, the delay-slot exclusion
  intact, not admitted to a section).
- **Exec** (`loader.cpp`): `quiesce_group(exec)` first, before anything is
  read from user memory, so nothing captured before the wait needs
  revalidating. Every refusal thaws the group (`ExecThaw` →
  `unfreeze`: clear `frozen`, release the claim, queue every runnable,
  probed or exiting sibling that is off its CPU); no sibling loses user
  progress. The leases of a quiesced group must all be inactive
  (`validate_quiesced_leases`). The commit check (`exec_superseded`): a
  pending exit set while the claim waited converts the claim to `exit` and
  the caller exits the group in place of the exec. `replace_image` ends the
  exec claim with its commit.
- **Exit** (`finish` in `src/user/runtime.cpp`, `quiesce_for_exit` in
  `smp.cpp`): a thread of the group claims exit for itself and waits; a CPU
  that runs no user thread (its kernel loop: `Kernel::kill_process`, boot, a
  legacy scan's delivery) claims as `kernel_claimer(cpu)`, an identity above
  every task id and distinct per CPU, and may wait, since nothing waits for
  it; a thread of another process never waits: it finishes a group none of
  whose threads is on a CPU at once (a fork child it undoes) and otherwise
  sets a pending exit. An exit claim taken while a pending exit is recorded
  exits with that status (`GroupClaim::owed`, then the signal state's
  record, as at the delivery point): an `exit_group(0)` that loses the race
  with an accepted SIGKILL reports the kill (Linux `do_group_exit` after
  `SIGNAL_GROUP_EXIT`).
- **Rescue kill** (`terminate_native_process`, called by
  `Kernel::kill_process` on CPU 0's kernel loop). It finishes the group under
  its own CPU's claim. If another CPU's transition holds the group (an exit,
  or an exec that the kill now ends at its commit check), it waits, releasing
  the BKL between checks, until that CPU completed the exit, so the rescue
  command can reap; on one CPU, where nothing could end that wait, it returns
  `busy`.
- **Pending exit and the delivery point.** `request_group_exit` (under the
  target's `task`): `pending_exit` with its status (SIGKILL overrides an
  earlier fatal default), a probe on every waiting thread, a queue entry for
  every parked one and a kick mask for the running ones; nothing when an
  exit already runs, and an exec claimer finds it at its commit check. Pick
  and `enqueue_locked` admit a pending-exit thread in any state, stopped or
  not, unless the group is frozen. Right after the pick (queue and legacy
  paths), before any resume, the picked thread runs `deliver_pending_exit`:
  `finish` with the recorded status, which quiesces the group.
- **Signal generation** (`jobctl.cpp`). SIGKILL, a native fatal default and
  the Linux `complete_signal` fatal shortcut keep their record in the signal
  state (`group_kill`, `deferred_fatal`) and set the pending exit
  (`jobctl_condemn`); a stopped native process gets it at SIGCONT. The
  step-level `private_process_signal_fatal` pass is deleted (spec 4.8).
- **Generation lock order** (spec 4.7). The stop family and SIGCONT are
  generated under the task table (rank 46); so are a group send's
  enumeration and deliveries (`send_process_group_signal`,
  `send_kernel_group_signal`), and the default stop applied by delivery or
  settle (`private_jobctl_stop`). Under it each task lock is taken in turn
  (stop, continue, probe, pending exit), never two at once, and the
  parent's (its SIGCHLD and probe) only after the target's was released. A
  hold is taken only when this CPU does not hold the table already;
  the scheduler's own task-table holds are re-entrant the same way.
- **Probes** (spec 4.7). A queued signal probes the process's waits
  (`private_signal_probe`): after `private_generate_signal` accepts one,
  after a thread-directed timer signal, after a CLD_STOPPED/CLD_CONTINUED
  and after an exit's SIGCHLD to the parent. A waiting thread whose wait a
  pending signal interrupts is probed; a parked one is queued (a stopped or
  frozen group keeps the probe for its continue or thaw), an armed one is
  queued by its `put_prev`.
- **Interruptible class with the parked-only shadow.** The owner records,
  when it parks (`settle_park` before the step's final `put_prev`), whether
  a signal can interrupt its wait (`WaitClass`, read from its own wait
  objects) and probes itself when a pending signal already interrupts it
  (the owner check at park). Generation's "qualified" decision and the
  timer admission use the recorded class (`private_wait_interruptible`); for
  a parked thread the old predicate `private_signal_admit_blocked` runs as its
  shadow, and a disagreement counts `admit_shadow_mismatch`. Because the
  owner records the class with that same predicate at park, the shadow
  compares the predicate with itself on the same objects at two times: it
  checks that a parked thread's wait objects did not change (only its owner
  or a quiescer may change them), not that an independently recorded class
  agrees with the predicate. `admit_shadow_mismatch = 0` is evidence for that
  invariant only.
- **The signal audit.** The per-step probe scan (`probe_interrupted_waits`)
  is gone. Every 256th CPU 0 pass, next to the queue audit, the same scan
  runs as an audit (`audit_signal_probes`): a parked, unprobed thread whose
  wait a pending signal would interrupt is a missed probe, repaired and
  counted in `audit_repairs`.
- **Job control** (spec 4.9). `stop_process` collects the CPUs running a
  thread of the process under its task lock and kicks them after unlocking;
  `continue_process` queues every off-CPU thread that is runnable, probed or
  exiting. `stopped_processes()` is a scan of the slots' stop flags
  (`stopped_count_` is gone).
- **Lifetime operations** (spec 4.12). Fork publishes its child with
  `publish_fork`: under the task table, the parent's task lock refuses a
  parent with a pending exit or an exit claim; the child is undone and the
  call stays pending (the parent exits at its next pick). Before the
  publication, a Linux parent whose calling thread has an unblocked pending
  signal with a terminating default (a `kill(-pgrp, SIGTERM)` queued while
  it ran user code) is condemned (`private_fork_condemn`: `deferred_fatal`
  and the pending exit, as generation's fatal shortcut), so the child is
  undone (Linux `copy_process` refuses a fork with a signal pending). A
  thread can no longer join a group with a pending exit or a transition
  (`create_thread_prepared` gives `interrupted`).
- **Deleted**: `prepare_eviction`, `commit_eviction`, `evict_thread`,
  `validate_evicted_leases`, `EvictionPlan`, `cpu_entries` and their
  `revoked` flag; the step's `no_process` path for a revoked entry; the
  `OK_NATIVE_EXEC_EVICT` line. `retire_native_thread` keeps its hotplug
  record retirement (`cpu_control_retire`, WP1) and loses the eviction;
  exec keeps dropping every old thread's hotplug record at its commit.

## Deviations from the specification

- **Signal queues stay under the BKL.** The spec queues the signal and
  decides its effects under the target's `task`. The signal state
  (`private_signal_actions`, the per-thread and shared queues, the action
  tables) is still written under the BKL; only the scheduler-side effects
  (stop and continue flags, probes, pending exit, queue entries) take the
  `task` lock, inside the order above. The predicates generation evaluates
  (the interruptible class's shadow, `private_signal_interrupting`) run
  before the probe takes `task`, not under it.
- **The interruptible class is recorded at park, not at arm.** The arm sites
  (22) do not know yet whether a signal can interrupt their wait (that
  depends on the wait object and the captured syscall origin, which they
  create after arming). The owner records the class from its own objects
  when it leaves its CPU blocked, which is the last moment it owns them
  alone. A wait not parked yet (`unknown`) is decided by the predicate, as
  before.
- **Resume order unchanged.** A probed pick runs the existing per-family
  signal completions in their order (`resume_interrupted_wait`), not a
  dispatch selected by `wait_reason`.
- **SIGKILL does not clear the stop flag.** Pick and enqueue admit a
  pending-exit thread of a stopped process instead; the exit clears the flag.
- **A foreign error finish** (another process's thread finishing a group,
  for example a fork undoing its child) acts at once when no thread of the
  group is on a CPU, through a claim that is refused while one is
  (`quiesced_only`); otherwise it becomes a pending exit.
- **Fork restart.** An aborted fork publication leaves the call pending
  rather than restarting it: its parent exits before it returns to user mode.
  The spec aborts on any "pending group-fatal signal"; the code condemns a
  parent with a pending terminating default at publication and then aborts
  on the pending exit. A pending *caught* signal does not abort the fork (no
  `-ERESTARTNOINTR` restart exists): the child is published and does not
  receive a group signal sent before it existed (left for WP7b).
- **Rescue kill waits.** Spec 4.8 has `Kernel::kill_process` use the
  pending-exit path. It still finishes the target synchronously on CPU 0's
  kernel loop, which runs no thread of any group and holds only the BKL,
  released while it waits, so no wait chain can form through it; with a
  transition of the group in flight it waits for that CPU's exit to
  complete (above), keeping the rescue command's immediate reap.
- **The shadow checks unchanged objects, not an independent class** (see
  the interruptible class above): recording the class at the 22 arm sites
  is left for WP7b, which deletes the predicate.
- **No `exiting` run flag** is set; the exit claim marks the transition.
- **No `admit_shadow_skipped` counter**: armed threads are decided by their
  record without a shadow, and no new evidence field was added.

## Tests

- New `test_group_transition.py` (`tests/host/group_transition_tests.cpp`,
  ten profiles, `OK_HOSTED_LOCK_MODEL` and `OK_LOCK_DEBUG`, real task, task
  table, run queue and kernel locks; host threads model CPUs): exec with
  three siblings in user mode on other CPUs while the claimer holds the
  BKL (pick under the BKL, user mode without it, the post-user relock and
  its frozen check, `put_prev`), with the exec-failure thaw each round and a
  second exec refused (`exec_held`); exit beats exec (a kill under the BKL
  the claimer released, then the commit check); a kill of an all-parked
  process racing a wake (every thread queued once, pick admits blocked
  ones); SIGCONT with a pending probe (deterministic and raced against
  stop/continue); fork against a group kill under the task table (no
  published child of a killed parent escapes); stop/continue against park
  and wake (the audit finds nothing lost).
- `test_native_exec_eviction.py` (`native_exec_eviction_tests.cpp`, ten
  profiles) now runs the actual runtime with a host thread as CPU 1: exec
  with a sibling, and with the leader, in user mode on CPU 1 while the
  caller holds the BKL; every refusal thaws the sibling with its frame and
  its trapped syscall; a breach after the quiesce stops the runtime and
  thaws the group; exit beats exec (a kill from CPU 1 during the wait);
  exit_group with a sibling on CPU 1; the leaseless leader and the hotplug
  request cases.
- The signal models that applied a fatal decision with the deleted pass now
  deliver the pending exit as the target's next pick would
  (`tests/host/native_pending_exit.hpp`): native group signals, process
  signals, signal production, job control and the child-exit runtime models.
- The UDP dispatch integration model (`test_net_runtime.py`) killed a
  process and expected the next step to have found nothing to run; the
  step now picks the killed thread, which exits the group, and the step
  after it reaps the orphan and finds nothing.

## Validation

All runs are QEMU TCG on the pinned SysToolchain QEMU 10.2 (`scope=hardware`
means the emulated machine); model tests print `scope=model`. Kernel
sources (`src`, `include`) at 2c85820, unchanged since; b5033e1 changes a
host test only. Logs under `smp-scaling/waveB/WP4d/logs`.

- Hosted: the full `scripts/hosted_suite.py` with the toolchain root: 171
  passed, 3 failed, 59 not run (inputs, quarantine, ext4 fixture),
  including the new `test_group_transition.py` and the rewritten
  `test_native_exec_eviction.py` on all ten profiles. Of the three:
  `test_net_runtime.py` failed on the UDP dispatch model's kill expectation
  (fixed above; the runner then passed 18 of 18); `test_block_cache.py`
  failed in xmake before compiling (a missing `.xmake` stats file) and
  passed alone; `test_wait_park_race.py` (WP4a) failed on ppc at its
  coverage check that both park/complete interleavings occurred, under the
  suite's load, and passed alone on all ten profiles. An earlier suite run,
  before 9e89daf, failed 13 signal and wait runners; all pass now. The
  quarantined runners fail at the same check or build step as on the
  baseline d7fde31 (`linux_udp_sockname` passes on both).
- Kernel: debug `xmake test` at `-smp 1` on all ten profiles and at 2 and 4
  on x86_64, aarch64 and rv64 (16 of 16 `OK_TEST_PASS`); `legacy_select=y`
  at `-smp 1` on x86_64.
- OS (private copy of `wt/os-smp-scaling` b8dceae, unchanged; no OS patch):
  native distro boot to `OK_USERLAND scope=hardware acceptance=pass` on all
  ten profiles. On x86_64, aarch64 and rv64: `test_smp_stress.py
  --user-hotplug` in debug at 2 and 4 vCPUs and in release at 2 and 4, the
  futex probe at 2, `--smp_hotplug_test=y --require-hotplug` at 2 and
  `smp_sections=n` at 2; the CPU sysfs probe at 4 vCPUs on x86_64 and at
  one on arm32. Every run that passed has every check counter
  (`lock_violations`, the assertion counters, `admit_shadow_mismatch`,
  `quiesce_timeouts`, `audit_repairs`, `poll_audit_ready`) at zero,
  `unqueued=0`, and `max_lock_depth` 5 in debug kernels and 0 in release
  ones. `--smp_hotplug_test` ran 439 to 791 cycles with no failure.
- Failed runs, each rerun and passed: aarch64 release at 2 vCPUs stopped
  in boot on posix-check's 200 ms timing check of its child (seen before
  WP4d, WP1 and integration); x86_64 debug `--user-hotplug` at 2 vCPUs
  failed one online write with EIO (the restarted CPU did not come online
  within 2 s; `max_us` 3.2 s; WP5 saw the same failure on its own base);
  x86_64 `--require-hotplug` at 2 failed one restart with "x86 secondary
  TSC was not synchronized" while `deadline_late_max_us` read 9.8 s, a
  host stall (four QEMU guests and host builds ran together). One x86_64
  SDK export failed in xmake (`boot16.bin` not found, as WP4a and WP4b
  saw). Every check counter was zero in the failed runs too.
- Evidence in those runs: `OK_SMP_WAKE exit_deliveries` 36 to 39 per stress
  run at 4 vCPUs (the stress probe's killed hotplug writers exit at their
  group's pick); `group_waits=0` and `frozen_skips=0` everywhere: no OS
  workload execs or exits a group while a sibling runs on another CPU, so
  the quiesce wait and the frozen check have model evidence only
  (`test_group_transition.py`, `test_native_exec_eviction.py`).
