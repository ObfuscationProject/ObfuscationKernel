# Process Scheduling

`ok::sched::Scheduler` owns process control blocks and delegates policy to
`SchedulerPolicy`. The default policy is `RoundRobinPolicy`.

`SchedulingMode` selects the scheduler policy envelope:

- `cooperative`: explicit yield-only scheduling.
- `round_robin`: baseline global round-robin dispatch.
- `per_cpu_round_robin`: SMP-aware current-process tracking per CPU.

The baseline creates an idle process during boot and marks it runnable.
`ModuleManager` can also create the `kmodd` background kernel process for
non-core modules whose manifest uses `ModuleExecution::kernel_process`; those
modules stay in kernel space but are visible in scheduler/process diagnostics.
Future work should add priorities, sleeping queues, CPU affinity, and SMP run
queues.
