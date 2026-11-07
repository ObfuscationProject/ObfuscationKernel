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

## Per-CPU run queues

`SchedulerState` owns one `CpuRunQueue` per CPU (`include/ok/sched/run_queue.hpp`):
a bounded FIFO of `ThreadRef{pid, tid}` identities, never PCB/TCB addresses,
because process storage compacts on reap and task identities are never reused.
Each queue has its own lock, selected at build time through `SchedulerRunQueue`
and constrained by the `RunQueue` concept; every queue operation takes only that
lock, so no path holds two queue locks. Queues are a mechanism: nothing enqueues
implicitly, and the native runtime still picks threads by its own scan.

- `set_online_cpus(mask)` keeps CPU 0 online and refuses unconfigured CPUs; the
  default is CPU 0 only. `configure_cpus` intersects the online set with the new
  count and places entries of removed CPUs again.
- CPU hotplug uses `offline_cpu(cpu)` and `online_cpu(cpu)`. A secondary CPU
  that leaves the online set has its queue popped and every live entry placed
  again on an online queue, and stale entries are dropped, so no thread is lost
  or queued twice. Its current slot stays until that CPU vacates it.
  `online_cpu` only sets the bit again, and the CPU starts with an empty queue.
- `enqueue_thread` places a runnable thread once (`queued_cpu`); a thread already
  queued or on a CPU is left alone. `select_cpu` prefers the last CPU while it is
  online and allowed, then the allowed online CPU with the shortest queue (lowest
  id on a tie), and otherwise CPU 0. It never picks an offline CPU. CPU 0 runs such
  an entry only when the affinity names no configured CPU. Otherwise the entry
  waits on CPU 0's queue until a CPU it allows comes online and dequeues or
  steals it.
- `dequeue_thread(cpu)` pops the CPU's own queue, discarding stale entries
  (threads that blocked after they were queued) and moving entries whose
  affinity changed. An empty queue steals the oldest eligible entry from the longest
  other online queue, try-locking each victim after releasing its own lock.
- `mark_on_cpu` dispatches a thread on a vacant online CPU and refuses a thread
  already on another CPU; `clear_on_cpu` vacates it. `on_cpu` mirrors each CPU's
  current slot and replaces the per-CPU scan that guarded double dispatch.
- Thread exit, exec, process exit and reap remove the affected entries, so each
  queue holds at most one entry per thread record and cannot overflow.
- A preempted thread leaves its process `running` while another of its threads
  is still on a CPU; with one CPU the transitions are unchanged.

`python3 -B scripts/test_run_queues.py --sanitize` is the hosted model suite
(`OK_RUN_QUEUES scope=model`). Host threads stand in for CPUs in its contention
case; it is not AP start, IPI or hardware SMP evidence.
