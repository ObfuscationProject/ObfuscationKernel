# SMP scaling WP4a: task lock and selection

Selection package of the smp-scaling work (spec sections 4.1, 4.2, 4.10 and
4.14). Every thread transition now runs under its process slot's `task`
lock, and the native step takes threads from the per-CPU run queues on all
ten profiles, UP and `-smp 1` included. The step itself still runs under the
big kernel lock (BKL); the task and run queue locks nest above it. Baseline:
375a177 (WP2 on the Wave A integration head). The module ABI is not bumped.

## What it adds

- **Task lock on every transition** (`src/sched/scheduler.cpp`). Arming a
  wait (`register_current_wait`), completing one (`complete_wait`), pick,
  put_prev, clone (`install_thread`), `set_runnable`, exit, stop, continue,
  affinity changes and the explicit queue API take the slot's `task` lock
  (rank 52); a run queue push or pop nests above it (`run_queue`, rank 56).
  Each TCB's `on_cpu` and `queued_cpu` have atomic mirrors in the process
  slot, as do the stop flag (`run_flags`) and the process affinity mask; they
  are stored with the field under the task lock and loaded lock-free only by
  the steal predicate. A placement write without the lock, a validated entry
  whose thread is on a CPU, a push no queue accepted and a mirror that
  differs from its field count `queue_asserts`.
- **`enqueue_locked`**: a `user_process` thread that may run (runnable, or
  blocked with the new `probe` flag) and is neither on a CPU nor queued is
  pushed to an online CPU its affinity admits (last CPU, else the shortest
  queue); a closed queue refuses and the next allowed CPU is tried. After the
  unlock the waker fences and sends a reschedule IPI if the target published
  itself idle (`notify_cpu`, only where locks are concurrent). Kernel_thread
  records are never queued by transitions; the kernel tick keeps dispatching
  them through its policy.
- **pick / put_prev** (spec 4.2). `pick(cpu)` pops its own queue (bounded by
  the count it found plus one), then steals from the longest other online
  queue it can try-lock, each once, with a predicate that loads only the slot
  mirrors. Every entry is validated under its task lock against its source
  queue: stale (slot reused) and superseded (the thread names another queue)
  entries are dropped, the latter without touching the field; an entry the
  affinity no longer admits is queued again. Pick occupies the `CpuSlot` and
  takes the CPU-slot pin (spec 3.2 (b)). `put_prev(cpu)` (the body of
  `vacate_cpu_slot`, so `activate(0)`, exit, exec, eviction and CPU leave use
  it too) makes a running thread runnable, queues it if it may run, drops the
  pin and stores `on_cpu` last.
- **Closed queues and the count mirror** (`run_queue.hpp`). A queue's closed
  flag follows "its CPU is offline" (`set_online_cpus`, `configure_cpus`,
  `offline_cpu`, `online_cpu`); `offline_cpu` closes it before it drains and
  places the entries again. The atomic count is the idle check's input.
- **Dekker idle** (`SchedulerState::enter_idle`, waker side in `notify_cpu`).
  The idle CPU stores its flag, fences (seq_cst) and loads the counts; the
  waker pushes, unlocks, fences and loads the flag. `halt` when all queues are
  empty, `own_work` (flag cleared, pick again) when its own queue holds
  entries, `steal_hint` when only another queue does: one more steal, then
  halt with the flag kept. A reschedule IPI sent after the fence is pending at
  the halt. The runtime's `runtime_idle` array is now the scheduler's.
- **Queue path on all ten profiles** (`src/user/runtime/entry.cpp`). The
  step picks with `pick`, settles a probed or interrupted wait with the
  existing per-wait signal completions (`resume_interrupted_wait`), promotes a
  probed thread only once its wait completed (`resume_current`), and puts the
  thread back with `activate(0)` as before. The `(pid, tid)` scan survives
  only behind `xmake f --legacy_select=y` (`OK_LEGACY_SELECT`, default off).
- **`derived_state()`**. A process stores only its lifecycle (created, live,
  zombie, exited); `state()` derives running, runnable or blocked from its
  live threads. The aggregate rewrites in `register_current_wait`,
  `complete_wait`, `clear_on_cpu`, `occupy_cpu`, `activate(0)`,
  `dispatch_selected`, `exit_thread`, `replace_image`, `set_runnable` and
  `block_process` are gone.
- **Per-CPU wakes**: completed waits count in a per-CPU, per-source row
  written by its CPU; `wakes(source)` sums them.
- **Step singletons (spec 4.14)**. The backend capture's sequence and the
  syscall-origin and VFS-continuation image, capture and request sequences
  are `smp::PerCpuSequence`s: each CPU counts its own values and tags them
  with its id, so they stay unique with no shared counter. Their records were
  already indexed by entry-stack lease slot, and so is `stranded_entry_stacks`.
  The selection cursors (`last_pid`, `last_tid`, `last_interrupted`,
  `runnable[]`) belong to the legacy scan.
- **Repair audit (spec 4.10)**. CPU 0 runs `audit_queues()` every 256th step
  in debug and release kernels (`OK_SMP_AUDIT passes`): under `task_table`
  and each `task` lock, a live user thread that may run but is neither on a
  CPU nor queued (orphan_runnable), or is blocked without a pending wait
  (stuck_completed), is made runnable and queued; each repair counts
  `audit_repairs`.
- **Evidence**: `OK_SMP_WAKE` now reports `remote` (a completion queued on
  another CPU), `ipis` (wakes of an idle CPU), `probes` (probed picks) and
  `steals`.

## Deviations from the specification

- **The signal-interruption rescan stays.** Signal generation does not set
  `probe` before WP4d. Instead each step marks every parked, unqueued thread
  whose wait a pending signal would interrupt (the eight predicates the scan
  evaluated, `probe_interrupted_waits`) and queues it; pick admits it blocked.
  Like the poll, deadline and futex rescans, which stay as the package
  requires, it runs under the BKL and goes with WP4d.
- **`set_runnable` and `block_process` stay** as thread-level operations
  (about a hundred hosted tests use them as setup); they no longer write an
  aggregate process state, and `set_runnable` queues what it makes runnable.
- **The task and run queue locks are `RankedSpinLock`s, not `FineLock`s**, on
  every profile, for WP2's reason: they sit in the exported scheduler layout
  and the SDK does not carry `OK_ARCH_SMP_START`.
- **Steals do not consume the pick budget.** The spec bounds the whole loop by
  the own count plus one; here that bound applies to own pops, and each other
  online queue is tried once, so an empty own queue still tries every victim
  (as `dequeue_thread` did).
- **`steal_hint`.** The spec's idle side reloads every stealable queue and
  picks again whenever one is non-empty. Entries another CPU's affinity holds
  would then keep an idle CPU stepping, and taking the BKL, forever; here a
  non-empty other queue allows one more steal attempt and the CPU then halts
  with its flag published. A push to its own queue always sees the flag; a
  push to another queue wakes that queue's CPU.
- **Per-CPU tagged sequences instead of `PerCpu<SharedNativeCapture>` and
  per-lease counters.** Duplicating the capture's 128-record table per CPU
  costs about 64 KiB of initialized data for nothing: the records are lease
  indexed and only the lease's CPU touches one. Tagging keeps every value
  globally unique, so no comparison changes meaning. The hosted fixtures that
  injected exhaustion through the old `u64` counters now exhaust the calling
  CPU's count.
- **Not changed here:** origin and continuation records are still allocated
  and retired under the BKL, not the task lock; `tcp_immediate_runtime` stays
  a BKL-only global (its family is `bkl_required` until WP7a, and there is no
  `net_stack` lock yet); the `poll` part of the audit belongs to WP4b with
  the wait channels (`poll_audit_ready` stays 0).
- **Hosted step models keep their cursor writes.** Forty-two hosted tests
  drive `native::step`; 24 of them reset `last_pid`/`last_tid` first. In the
  queue build those names are kept (marked unused) so the tests compile; the
  step ignores them. The tests run the queue path.

## Validation

(filled in below)
