# Process Scheduling

`ok::sched::Scheduler` owns process control blocks and delegates policy to
`SchedulerPolicy`. The default policy is `RoundRobinPolicy`.

`SchedulingMode` selects the scheduler policy envelope:

- `cooperative`: explicit yield-only scheduling.
- `round_robin`: baseline global round-robin dispatch.
- `per_cpu_round_robin`: SMP-aware current-process tracking per CPU.

The baseline creates an idle process during boot and marks it runnable.
`DriverManager` registers started drivers as background kernel processes named
`drv:<driver-name>`. `ModuleManager` registers non-core modules whose manifest
uses `ModuleExecution::kernel_process` as `mod:<module-name>` background kernel
processes. These processes stay in kernel space and are primarily visible
through scheduler/process diagnostics such as debug-shell `ps aux`.
Future work should add priorities, sleeping queues, CPU affinity, and SMP run
queues.
