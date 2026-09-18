# Canonical process resources

`Scheduler` is the sole owner of process and thread records. `Process` and
`Thread` are compatibility aliases for `ProcessControlBlock` and
`ThreadControlBlock`. `ProcessManager` is a non-owning facade and requires an
explicit `Scheduler &`; constructing it does not create another process table.
Scheduling policy selection remains a constrained template.

Each process owns credentials, parent/child links, descriptor slots, cwd, umask,
heap/mmap metadata and an address-space description. Each thread owns its CPU
context, TLS and wait state. The native `UserFrame` belongs to the same thread;
its address-space handle and page ownership list belong to the same process.
The native runtime must release physical backing and destroy the hardware space
before reaping. Reaping a process that still owns native pages returns `busy`.

Descriptor slots hold handles into the scheduler's open-file-description pool.
`fork` and `dup` retain those descriptions, sharing offsets and status flags while
keeping descriptor flags and tables independent. Last close releases the open
description. `exec` closes only CLOEXEC descriptors and preserves cwd/identity.
Legacy descriptor-table metadata helpers reject copying or closing live
descriptions; those operations must go through the scheduler to maintain counts.
ELF/map validation completes before any existing image is replaced. Exit records
one status and releases descriptors once; wait consumes the zombie once.
Normal exit codes and termination signals are stored separately. `wait_status()`
encodes the low eight bits of a normal exit code, including negative input, or
the terminating signal without interpreting a negative exit as a signal.

Exit immediately reparents children to the kernel service reaper (parent zero).
Live orphans continue running. At native-loop service boundaries,
`reap_orphans()` removes exited parent-zero processes after their native backing
has been released. It never consumes children whose parent is still alive, and
it runs outside dispatch while no PCB/TCB pointers will be reused after removal.
This bounded policy reports `getppid() == 0` for orphans; it does not implement
init adoption or subreapers.

`PosixService::activate(pid)` selects the executing resource context;
`activate(0)` returns to kernel services. The runtime uses `fork`, `exec`, `exit`
and `wait` on that same service. Kernel diagnostics explicitly use context zero
so a shell identity switch cannot rewrite the scheduler's last selected daemon.
GUI input, extension callbacks and post-scheduling refresh use the same service
context. The native loop also returns to it when no native task is runnable;
current native bridges pin their process to CPU 0 until AP execution exists.

Pipes use bounded shared buffers. Empty reads block while writers exist and
return EOF after the last writer closes; full writes block, and writes with no
reader return `broken_pipe`. Read/write/close wake the matching blocked threads.
Wait blocks the caller until child exit. A native runtime retries the saved
syscall once its canonical thread becomes runnable; it does not allocate a
second waiting-task table.

Host regressions exercise ownership identity, resource separation, shared
open-file offsets, descriptor retention, exec rollback, CLOEXEC, single reaping,
pipe EOF and wakeup, repeated orphan reaping, exit/signal status and malformed ELF
rejection:

```sh
python3 scripts/test_process_resources.py
python3 scripts/test_static_dispatch.py
python3 scripts/test_archive.py
python3 scripts/test_kernel_services.py
```

The first two retain all ten architecture profiles. These are model and host
checks; native execution acceptance is performed separately in QEMU. Large
scheduler model fixtures use static storage instead of consuming the 1 MiB
bootstrap stack.
The service regression invokes real GUI callback boundaries with a host native
execution stub and verifies that selected or blocked process identities survive.
