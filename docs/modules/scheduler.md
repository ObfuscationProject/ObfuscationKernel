# Process Scheduling

`ok::sched::Scheduler` owns process/thread control blocks and delegates process
selection to a plain C++ `SchedulerPolicy` object. Policies are ordinary OOP
strategy objects, not kernel modules or service registrations; future algorithms
can derive from the interface and be injected through the constructor or
`set_policy()`. The default policy is `RoundRobinPolicy`, now priority-aware
while preserving round-robin fairness among equal-priority runnable processes.

`SchedulingMode` selects the scheduler policy envelope:

- `cooperative`: explicit yield-only scheduling.
- `round_robin`: baseline global round-robin dispatch.
- `per_cpu_round_robin`: SMP-aware current-process tracking per CPU.

The baseline creates an idle process during boot, adds one idle thread per
configured CPU, and marks it runnable. The kernel tick walks every online CPU and
calls `schedule_next_on_cpu(cpu)`, so secondary cores advance their own current
PID/TID and dispatch counters. Process records carry priority, CPU affinity,
dispatch counters, last CPU, and per-thread dispatch accounting.
`Scheduler::create_thread()` adds additional runnable threads to an existing
process, and `current_tid(cpu)` exposes the selected thread on each CPU.
`cpu_stats(cpu)`, `cpu_usage_percent(cpu)`, and `process_usage_percent(pid)`
provide the fixed-capacity accounting used by the kernel task manager.

Subsystems that need to create schedulable work should enter through
`Scheduler::spawn(ScheduleRequest)`. The request applies the process name,
initial context, background/user credentials, priority, and CPU affinity in one
validated operation, so later loaders and runtime services do not need to stitch
together process creation policy by hand.

`DriverManager` registers built-in drivers as `drv:<driver-name>` background
kernel daemon processes. `ModuleManager` registers non-core modules whose
manifest uses `ModuleExecution::kernel_process` as `mod:<module-name>`
background kernel processes through the generic spawn interface. Scheduler
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
credentials active at launch time; `fm` launched from the shell blocks only its
launching `oksh` until the file manager exits.
Task monitor applications follow the same rule: `taskman gui` uses `tm:<user>`
and `top gui` uses `top:<user>`, while their TUI modes render synchronously in
the shell without creating module records. GUI monitor auto-refresh is throttled
by the kernel tick; user-driven redraws such as open, resize, and scroll still
render immediately.
Future work should replace the simulated dispatch counters with timer-derived
runtime deltas and add real SMP run queues once architecture code starts running
kernel threads concurrently.
