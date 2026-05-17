# Process Scheduling

`ok::sched::Scheduler` owns process control blocks and delegates policy to
`SchedulerPolicy`. The default policy is `RoundRobinPolicy`.

The baseline creates an idle process during boot and marks it runnable. Future
work should add priorities, sleeping queues, CPU affinity, and SMP run queues.

