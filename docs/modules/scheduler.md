# Process Scheduling

`ok::sched::Scheduler` is the build-time alias
`BasicScheduler<RoundRobinPolicy>`. `SchedulerPolicy` is a C++ concept requiring
`pick_next(processes, current_pid, cpu) -> Result<ProcessId>`. Alternative
algorithms instantiate `BasicScheduler<OtherPolicy>`; they do not inherit a
virtual interface. Each scheduler owns its policy by value. `set_policy()`
replaces policy state of the same concrete type, without switching algorithms
through a runtime base pointer. The default policy is priority-aware and keeps
round-robin fairness among equal-priority runnable processes.

`SchedulerState` provides nonvirtual process/thread storage and shared dispatch
bookkeeping. The template directly invokes its concrete policy and then this
shared mechanism, avoiding both a vtable call in task selection and duplicated
storage code for each policy. These remain scheduling **models**: selecting a
PID/TID does not restore CPU registers, change stacks, or switch address spaces.
`SchedulerState` also owns the console and PTY terminals, the terminal
namespace and the per-process stop/continue overlay; see the
[terminal contract](tty.md).

`SchedulingMode` currently records the requested scheduling model; it does not
enable a hardware scheduling mechanism:

- `cooperative`: explicit yield-only scheduling.
- `round_robin`: baseline global round-robin dispatch.
- `per_cpu_round_robin`: SMP-aware current-process tracking per CPU.

The baseline creates an idle process during boot, adds one idle thread per
configured CPU, and marks it runnable. The kernel tick walks every online CPU and
calls `schedule_next_on_cpu(cpu)`, so per-CPU records advance their current
PID/TID and dispatch counters in the same execution thread. This does not start
secondary processors. Process records carry priority, CPU affinity,
dispatch counters, last CPU, and per-thread dispatch accounting.
`Scheduler::create_thread()` adds additional runnable threads to an existing
process, and `current_tid(cpu)` exposes the selected thread on each CPU. CPU
accounting is owned by the scheduler rather than by `top`: idle and passive
kernel daemon records are sampled for scheduling state but are not counted as
busy work. `cpu_stats(cpu)`, `cpu_usage_percent(cpu)`, and
`process_usage_percent(pid)` provide the fixed-capacity accounting used by the
kernel task manager.

Subsystems that need to create schedulable work should enter through
`Scheduler::spawn(ScheduleRequest)`. The request applies the process name,
initial context, background/user credentials, priority, and CPU affinity in one
validated operation, so later loaders and runtime services do not need to stitch
together process creation policy by hand.

`DriverManager` registers built-in drivers as `drv:<driver-name>` background
kernel daemon processes. `ModuleManager` registers non-core modules whose
manifest uses `ModuleExecution::kernel_process` as `mod:<module-name>`
background kernel processes through the generic spawn interface. CPU-rendered
modules such as `kernel-gui` can request per-CPU worker threads, while passive
driver/module daemons remain visible without inflating CPU usage. Scheduler
process records now carry credentials;
kernel-space process records are visible in debug-shell `ps aux` only while the
shell session is using the `kernel` debug user. The debug shell `kill <pid>`
command forcefully removes non-protected process records and asks kernel-owned
UI records such as `fm:<user>` to close their surfaces; kernel-space records
require the `kernel` debug-shell user. Supervised `drv:*` and `mod:*` daemon
records are recreated after a kernel-user kill, and each restart is logged to
the console and `/tmp/kernel.log`. Each GUI debug shell registers as its own
`oksh` process with isolated session credentials. UI processes created by the
`kernel` debug user are recorded as kernel threads with address-space ID 0.
UI processes created by `root` or another normal user are recorded as
`user_process` instances with a distinct address-space ID and user-mode context.
GUI file manager launches create distinct `fm:<user>` processes using the
credentials active at launch time; `fm` launched from the shell makes only its
launching `oksh` wait for new commands until the file manager exits, while the
shell process remains runnable for terminal I/O.
Task monitor applications follow the same rule: `taskman gui` uses `tm:<user>`
and `top gui` uses `top:<user>`, with each GUI launch getting its own scheduler
process and surface. Their TUI modes render synchronously in the shell without
creating module records. GUI monitor auto-refresh is throttled by the kernel
tick; user-driven redraws such as open, resize, and scroll still render
immediately. The GUI monitor samples scheduler counters per refresh, so displayed
CPU percentages are interval deltas rather than ever-growing boot averages.
Future work should replace the simulated dispatch counters with timer-derived
runtime deltas.

`python3 scripts/test_static_dispatch.py` runs host model tests under all ten
architecture profile macros. It exercises the default policy and an independent
stateful policy, priority/affinity, copied policy ownership, invalid CPU IDs,
and compile-time rejection of a policy with an incompatible signature. These
tests are not hardware preemption or SMP evidence.

## Task storage

Processes and threads live in fixed slots (`include/ok/sched/task_table.hpp`,
smp-scaling WP2). Nothing moves or compacts, so a PCB or TCB pointer stays
valid memory for the scheduler's lifetime; what it names is checked by id.

- 64 `ProcessSlot`s. Each holds the slot's `task` lock, the `pid` and
  `lifecycle` words that any CPU may load (free, embryo, live, exiting, zombie,
  reaped), a pin count, the per-thread affinity words (`thread_affinity`, the
  sched_setaffinity masks) and the PCB. Allocation walks forward from the last
  allocated slot, so slot order follows creation order until it wraps. It
  claims a slot (embryo), builds the PCB in place and publishes the pid,
  holding `task_table` and then the slot's `task` lock (`claim_free_slot`,
  `publish_slot` and `release_slot`, which the task-table model drives as
  well). A free slot that still counts a pin is never claimed; it counts
  `pin_asserts`. `find(pid)` scans the 64 pid words under the kernel lock;
  a caller without it uses `pin(pid)`. `processes()` is a range over the
  [live, zombie] slots in slot order, and the `SchedulerPolicy` concept takes
  that range.
- `ProcessPin` (`SchedulerState::pin(pid)`) holds a [live, zombie] slot. Reap
  marks the slot reaped and frees it at once when no pin is held; otherwise
  the last unpin frees it. A reaped slot is invisible to lookups but keeps
  its pid and PCB. Reap no longer drains kernel sections and no longer
  refuses while UNIX receives or descriptor reservations are prepared
  elsewhere, because no other record moves.
- Each PCB has eight TCB slots; slot 0 is the group leader (tid == pid).
  `slot_state` is free, embryo (reserved, unused until clone prepares in
  place), live or exited. `live_threads()` yields live slots,
  `thread_records()` live and exited ones, `leader()` slot 0 whatever its
  state, `main_thread()` the leader while it lives and else the lowest live
  slot. Thread exit keeps an exited record; a later clone reuses the first
  exited record that holds nothing, and exec clears every sibling slot.
- `ThreadHandle{index = slot * 8 + thread, tid}` names a thread while that
  slot holds that tid (`thread_at`). `WaitTicket` carries the same index, so
  `complete_wait` finds its waiter without a search; a ticket armed outside
  `register_current_wait` (no index) is still found by its identities.
- Each CPU has a `CpuSlot`: pid, tid, PCB and TCB pointers and its dispatch
  counters, written by `occupy_cpu`/`vacate_cpu_slot`; occupying a CPU takes a
  pin on the process slot that vacating it drops. The POSIX service reads
  its caller from there (or from an explicit binding, which kernel sections
  set), never by searching the table.

Model tests: `scripts/test_task_table.py` (slot lifecycle, pins, ranges; a
host-thread race of pins against reap and reallocation with the real lock)
and `scripts/test_thread_lifecycle.py` (stale handles and tickets after slot
reuse, pins against reap, exited-slot iteration).

## Per-CPU run queues

`SchedulerState` owns one `CpuRunQueue` per CPU (`include/ok/sched/run_queue.hpp`):
a bounded FIFO of `ThreadHandle{index, tid}` entries, never PCB/TCB addresses
(see Task storage above). Each queue has its own `run_queue` lock (rank 56, a
`RankedSpinLock` on every profile so that the exported layout is one),
selected at build time through `SchedulerRunQueue` and constrained by the
`RunQueue` concept; every queue operation takes only that lock, so no path
holds two queue locks. A queue keeps an atomic mirror of its entry count,
which idle checks and victim choice load without the lock, and a `closed`
flag: the queue of an offline CPU refuses pushes.

### Task locks and placement (smp-scaling WP4a)

Every thread transition runs under its process slot's `task` lock (rank 52),
still inside the big kernel lock; the queue lock nests above it. Each TCB's
`on_cpu` and `queued_cpu` have atomic mirrors in its process slot, and the slot
mirrors the stop flag (`run_flags`) and the process affinity mask; they are
stored with the TCB field under the task lock and loaded without it only by
stealing. A write without the task lock counts `queue_asserts`.

A process stores only its lifecycle (created, live, zombie, exited);
`state()` is `derived_state()`: running, runnable or blocked as its live
threads are. No thread transition rewrites a process state.

Transitions queue a `user_process` thread whenever it may run and is neither
on a CPU nor queued (`enqueue_locked`, spec 4.2): completion of a parked wait
(`complete_wait`), `put_prev`, clone (`install_thread`), `set_runnable`
(spawn, fork, init), `continue_process`, exec (`replace_image` queues a
leader that was parked or had exited when a sibling called exec; a leader
that called exec itself is queued by the `put_prev` of its CPU), the probe of
an interruptible wait and the repair audit. `kernel_thread` records are never queued this way; the
kernel tick keeps dispatching them through its policy. A thread may run when
it is runnable, or blocked with `probe` set (a pending signal may interrupt
its wait), its process is live and not stopped. After the unlock the waker
fences and wakes the chosen CPU with a reschedule IPI if that CPU published
itself idle.

- `pick(cpu)` takes the next entry from the CPU's own queue (bounded by the
  count it found plus one), then steals the oldest entry another online queue
  holds for this CPU, try-locking victims in order of length; the steal
  predicate loads only the slot mirrors. Each entry is validated under its
  task lock against the queue it came from: a stale entry (slot reused) or a
  superseded one (the thread names another queue) is dropped and leaves the
  field alone; one the affinity no longer admits is queued again where it may
  run. A runnable thread becomes running; a probed one stays blocked until its
  resume completes the wait (`resume_current`). Pick fills the `CpuSlot` and
  takes the slot pin of spec 3.2 (b).
- `put_prev(cpu)` (and `vacate_cpu_slot`, which `activate(0)`, exit, exec and
  remote eviction use) turns a running thread back to runnable, queues it if
  it may run, drops the CPU-slot pin and releases `on_cpu` last.
- `enter_idle(cpu)` is the idle side of the handshake: it publishes the CPU
  idle, fences and loads the queue counts. `halt` when every queue is empty;
  `own_work` (the flag is cleared) when its own queue holds entries;
  `steal_hint` when only other queues do, so the caller may try one more steal
  and then halts with the flag published. A push to its own queue after the
  fence sees the flag and wakes it.
- `set_online_cpus`, `configure_cpus`, `offline_cpu(cpu)` and `online_cpu(cpu)`
  keep each queue's closed flag equal to "the CPU is offline". A CPU that
  leaves the online set has its queue closed and popped, and every live entry
  placed again on an online queue (stale entries are dropped), so no thread is
  lost or queued twice. `online_cpu` reopens the queue.
- Placement (`select_cpu`/`select_from`) prefers the last CPU while it is
  online and allowed, then the allowed online CPU with the shortest queue
  (lowest id on a tie), and otherwise CPU 0, never an offline CPU. CPU 0 runs
  such an entry only when the affinity names no configured CPU. A push a
  closed queue refuses tries the next allowed CPU.
- `audit_queues()`, run by CPU 0 every 256th step in debug and release
  kernels (spec 4.10), visits every live user process under `task_table` and
  then its `task` lock. A thread that may run but is neither on a CPU nor
  queued (orphan_runnable), or is blocked without a pending wait
  (stuck_completed), is made runnable and queued; each repair counts
  `audit_repairs` (OK_SMP_AUDIT), which the gates require to stay 0.
- Completed waits are counted per CPU and per source (`wakes()` sums them).
- The explicit `enqueue_thread`, `dequeue_thread`, `mark_on_cpu` and
  `clear_on_cpu` remain for the hosted models and the `legacy_select=y`
  scan; they take the same task locks.

The native step uses the queues on every profile, UP and `-smp 1` included
(queue 0 only on one CPU), so threads run in FIFO order. `xmake f
--legacy_select=y` keeps the earlier `(pid, tid)` scan of the step as the A/B
arm until it is deleted; the transitions above queue threads in both builds.

`python3 -B scripts/test_run_queues.py --sanitize` is the hosted queue model
(`OK_RUN_QUEUES scope=model`); `scripts/test_scheduler_waits.py` models the
queue-path transitions, probes, the idle check and the repair audit; and
`scripts/test_wait_park_race.py` races park against complete, steal against
re-enqueue and idle against a push on host threads with the real locks
(`OK_WAIT_PARK_RACE scope=model`). Host threads stand in for CPUs; none of
these is AP start, IPI or hardware SMP evidence.
