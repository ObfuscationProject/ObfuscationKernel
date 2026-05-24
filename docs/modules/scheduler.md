# Process Scheduling

`ok::sched::Scheduler` owns process control blocks and delegates policy to
`SchedulerPolicy`. The default policy is `RoundRobinPolicy`.

`SchedulingMode` selects the scheduler policy envelope:

- `cooperative`: explicit yield-only scheduling.
- `round_robin`: baseline global round-robin dispatch.
- `per_cpu_round_robin`: SMP-aware current-process tracking per CPU.

The baseline creates an idle process during boot and marks it runnable.
`DriverManager` registers started non-RAM-helper drivers as background kernel
processes named `drv:<driver-name>`. `ModuleManager` registers non-core modules
whose manifest uses `ModuleExecution::kernel_process` as `mod:<module-name>`
background kernel processes. Scheduler process records now carry credentials;
kernel-space process records are visible in debug-shell `ps aux` only while the
shell session is using the `kernel` debug user. The debug shell `kill <pid>`
command marks non-protected process records exited, exits all of their threads,
and asks kernel-owned UI records such as `fm:<user>` to close their surfaces;
kernel-space records require the `kernel` debug-shell user. GUI file manager
launches create `fm:<user>` background processes using the credentials active in
the debug shell at launch time.
Future work should add priorities, sleeping queues, CPU affinity, and SMP run
queues.
