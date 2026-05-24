# Process Scheduling

`ok::sched::Scheduler` owns process control blocks and delegates policy to
`SchedulerPolicy`. The default policy is `RoundRobinPolicy`.

`SchedulingMode` selects the scheduler policy envelope:

- `cooperative`: explicit yield-only scheduling.
- `round_robin`: baseline global round-robin dispatch.
- `per_cpu_round_robin`: SMP-aware current-process tracking per CPU.

The baseline creates an idle process during boot and marks it runnable.
`DriverManager` registers built-in drivers as `drv:<driver-name>` background
kernel daemon processes. `ModuleManager` registers non-core modules whose
manifest uses `ModuleExecution::kernel_process` as `mod:<module-name>`
background kernel processes. Scheduler process records now carry credentials;
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
Future work should add priorities, sleeping queues, CPU affinity, and SMP run
queues.
