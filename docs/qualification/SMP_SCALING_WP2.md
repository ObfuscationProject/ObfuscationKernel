# SMP scaling WP2: stable tasks and identity

Task-storage package of the smp-scaling work (spec section 3). It changes the
data layout only: every scheduler path still runs under the big kernel lock
(BKL), and nothing a user can see changes. Baseline: Wave A integration
a631bea (WP0a, WP0b, WP1, WP3 on main 5815814), merged with the integration
head 4bbabdf (kernel main fd8abee, net-socket-gaps, module ABI 82). The ABI
number is not bumped here.

## What it adds

- **Fixed process slots** (`include/ok/sched/task_table.hpp`). The compacting
  `StaticVector<ProcessControlBlock, 64>` is now `std::array<ProcessSlot, 64>`.
  A slot holds its `task` lock (rank 52), the `pid` and `lifecycle` words
  (free, embryo, live, exiting, zombie, reaped) that any CPU may load, a pin
  count, the per-thread `thread_affinity` words and the PCB. Allocation claims
  the next free slot after the last one (embryo), builds the PCB in place and
  publishes the pid, holding `task_table` (rank 46) and then the slot's `task`
  lock. `find(pid)` scans the 64 pid words; `processes()` returns a
  `ProcessRange` over the [live, zombie] slots, and the `SchedulerPolicy`
  concept takes that range (`RoundRobinPolicy` and the kernel-thread round
  robin walk slot positions).
- **ProcessPin** (`SchedulerState::pin`). Under the slot's `task` lock it
  rechecks pid and lifecycle and counts a pin. Reap marks the slot reaped and
  frees it when no pin is held; otherwise the last unpin frees it. Underflow
  counts `pin_asserts`. The CPU 0 hotplug service's CS2 now pins the writer's
  process and checks its wait under that `task` lock.
- **Fixed thread slots**. A PCB has `std::array<ThreadControlBlock, 8>`; slot
  0 is the leader. TCBs gain `slot_state` (free, embryo, live, exited). New
  accessors: `live_threads()`, `thread_records()` (live and exited, the old
  iteration), `live_count()`, `record_count()`, `leader()`, `main_thread()`,
  `thread_slot(i)`. Every `threads()` use (src, include and tests) is
  rewritten; `erase_at` and `push_back` on threads are gone. Exec clears the
  sibling slots in place.
- **Handles and tickets**. `ThreadHandle{u16 index, u32 tid}` (index = slot *
  8 + thread) in `run_queue.hpp`; `thread_at(handle)` validates the tid.
  `WaitTicket` gains `index`; `register_current_wait` fills it and
  `complete_wait` finds its waiter in O(1). The scheduler's run queues hold
  handles (`CpuRunQueue` takes the entry type as a template parameter);
  `dequeue_thread` still returns a `ThreadRef`.
- **CpuSlot and caller identity**. `current_by_cpu_`, `current_thread_by_cpu_`,
  `cpu_stats_` and the write-only `current_pid_` became one `CpuSlot` per CPU
  holding pid, tid, PCB and TCB pointers and the dispatch counters.
  `PosixService::section_callers_` became `cpu_callers_` (an explicit
  binding, which kernel sections set); without one the caller is the CPU
  slot. `getpid`, `gettid`, `resources`, `thread_local_state` and
  `user_credentials` read that caller. The uaccess resolver's owner lookup
  tries the CPU slot first.
- **Thread affinity** moved from `ThreadControlBlock::cpu_affinity_mask` to
  `ProcessSlot::thread_affinity[8]` (u32 words, stored by plain store),
  reached through `SchedulerState::thread_affinity`/`store_thread_affinity`.
  `effective_affinity` is no longer static.
- **Side tables**. `private_signal_actions` and `process_timers` are indexed by
  process slot with a stored-pid check (an entry made without the runtime
  kernel is still found by its pid). `poll_waits` and `futex_waits` are sized
  by the entry-stack lease capacity (128) instead of 64.
- **Deleted**: the reap shift, `smp::drain_all()` in reap, the reap veto on
  prepared UNIX receives and descriptor reservations, the PPC32 and MIPS64 n64
  relocation stage/commit hooks (the runtime functions; the arch readiness
  methods and their tests stay), and the backend capture's re-find after a
  user entry (it now checks the identities of the records it holds). The
  departed Linux UDP receive prune that ran in the pre-compaction hook runs in
  a renamed pre-reap hook.

## Deviations from the specification

- **`leader()` next to `main_thread()`.** About 300 uses of `threads()[0]`
  (fork, exec, the signal queue and altstack of the process, the shutdown
  owner, posix's TLS fallback) mean "the leader's slot, whatever its state":
  the process-wide records hang off it, and exec keeps it. They became
  `leader()`. `main_thread()` (spec 3.3: the leader, else the lowest live
  slot) exists for callers that want a running representative; no converted
  site needed it.
- **`thread_records()` as the default rewrite.** A `threads()` loop that
  filtered `state != exited` became `live_threads()` or `live_count()`; every
  other loop became `thread_records()`, which keeps the old iteration (live
  and exited records). Live-only rewrites of the remaining loops belong to the
  packages that put them under `task`.
- **Exec clears sibling slots** (free) instead of marking them exited. At
  `replace_image` the siblings' leases are escrowed and nothing else
  references them, so this equals the old `erase_at`, keeps record counts
  unchanged, and the slots are reusable at once. WP4d (exec with frozen
  siblings) can mark them exited instead if it needs the records.
- **Clone still prepares a local TCB copy**; the embryo thread state is
  defined but unused. The prepared TCB is copied into the slot as before; the
  new thread's affinity word is stored after creation (clone copies the
  caller's mask, fork the calling thread's). In-place embryo preparation
  belongs with WP7b, which runs prepare unlocked.
- **Task locks are `RankedSpinLock`, not `FineLock`.** `task` (rank 52) and
  `task_table` (rank 46) live in `SchedulerState`, which the Kernel SDK
  exports. The SDK does not carry `OK_ARCH_SMP_START`, so a `FineLock` would
  be a `RankedSpinLock` in an SMP kernel and the smaller `UpLock` in its OS
  consumer: two layouts of one class. Both are spin locks on every profile;
  on UP profiles they are never contended.
- **Slot mirrors.** Only `thread_affinity` moved into the slot. The `on_cpu`
  and `queued_cpu` atomic mirrors, `run_flags`, `affinity` and the lineage
  mirrors have no lock-free reader before WP4a, so they are not added yet.
- **`WaitQueue` keeps the registration pointer** with its ticket. The
  pointer is into a TCB slot that never moves, so it stays valid; liveness is
  still `registration->pending(ticket)`. Resolving the ticket through the
  scheduler under `task` is WP4a/WP4b work, when the queue is read without
  the BKL.
- **`ThreadHandle` is `{u16 index, u32 tid}`** as in spec 3.3. The package
  brief's `{slot, index, generation}` is not needed: tids are never reused.
- **`cpu_callers_` falls back to the CPU slot.** There is no pick/put_prev
  yet, so the binding the scheduler writes in `occupy_cpu` and clears in
  `vacate_cpu_slot` is the CPU slot itself; `cpu_callers_` (the renamed
  section binding) overrides it. The section API is renamed
  `bind_cpu_caller`/`unbind_cpu_caller`. No CPU-slot pin is taken: pick and
  put_prev take it in WP4a.
- **`find_thread_by_tid` and `processes()` readers do not take
  `task_table`.** Only slot allocation and release take it; every reader still
  runs under the BKL. The rescue task manager, status and shell readers are
  unchanged apart from the range type.
- **Slot allocation walks forward** from the last allocated slot (next fit),
  so iteration order follows creation order until the table wraps, close to
  the old compacting vector. Spec 3.2 does not fix the policy.
- **Side tables keep a pid fallback.** Signal actions and process timers are
  slot indexed, but an entry made without the runtime kernel (hosted tests)
  is found by scanning its stored pid. `poll_waits` and `futex_waits` are
  sized by lease capacity but still allocated by scan, not by lease slot; the
  poll and futex object pools keep 64 objects.
- **Reap hooks.** The pre-compaction hook survives as
  `private_signal_before_process_reap` because it also retires departed
  Linux UDP receives; the post-compaction hook is gone, so
  `OrphanReapProgress::failed_after_erase` can no longer become true. The
  tests that injected a post-erase failure now check the pre-reap failure
  and its retry only.
- **Tests that asserted the old layout** now assert the new one: PCBs keep
  their address across another process's reap (linux_process,
  native_syscall_origin, scheduler_waits, run_queue), a live receiver or
  accept owner is refused reap with `invalid_argument` rather than the
  global `busy` veto and another process reaps meanwhile (unix_rights,
  unix_path_owner, and the three TCP accept tests; `test_net_runtime.py` builds
  `tcp_accept_transaction_tests`, no runner builds the other two), and
  the shared signal queue reuse test cycles processes until the replacement
  takes the retired slot.

## Validation

All runs are QEMU TCG on the pinned SysToolchain QEMU 10.2 (`scope=hardware`
means the emulated machine); model tests print `scope=model`. Kernel sources
at 0e72e0d (after the merge of the integration head 4bbabdf).

- Hosted: the full `scripts/hosted_suite.py` with the toolchain root, 170
  passed and 0 failed, including the new `test_task_table.py` (slot
  lifecycle, pins, ranges, and a host-thread race of pins against reap and
  reallocation with the real task lock) and the updated
  `test_thread_lifecycle.py` (stale handle and ticket after slot reuse, pin
  against reap, exited-slot iteration), `test_child_autoreap_scheduler.py`
  and `test_run_queues.py`. That run predates a comment-only header change;
  those runners plus `test_scheduler_waits.py` and `test_smp_locks.py` were
  rerun on the final sources. The five ext4 fixture runners
  (`--ext4-fixture`) passed. `scripts/test_net_runtime.py --sanitize`: all 18
  socket runtime suites passed.
- Kernel: debug `xmake test` at `-smp 1` on all ten profiles, and at `-smp 2`
  and `-smp 4` on x86_64, aarch64 and rv64.
- OS (private copy of `wt/os-smp-scaling` b8dceae, unchanged): native distro
  boot to `OK_USERLAND scope=hardware acceptance=pass` on all ten profiles;
  on x86_64, aarch64 and rv64, `test_smp_stress.py --user-hotplug` debug at 2
  and 4 vCPUs and release at 2, the futex probe at 2,
  `--smp_hotplug_test=y --require-hotplug` at 2 and `smp_sections=n` at 2;
  the CPU sysfs probe at 4 vCPUs on x86_64 and at one on arm32. Every run
  passed with every check counter (`lock_violations`, the assertion counters,
  `admit_shadow_mismatch`, `quiesce_timeouts`, `audit_repairs`,
  `poll_audit_ready`) and `unqueued` at zero; `max_lock_depth` was 5 in debug
  kernels.
