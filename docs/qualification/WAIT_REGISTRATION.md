# Serialized wait registration prototype

This unexported source prototype adds single-completion wait ownership to the
SDK21 source baseline. It changes the exported thread layout and must receive a
new C++ SDK generation before any SDK export or distribution linkage. It does
not change native C syscall numbers or wire layouts.

Each blocked thread owns a registration identified by process ID, thread ID and
a monotonically increasing generation. Registering an already active wait fails
without replacing its owner. Ready, timeout and cancellation compete for one
terminal transition. A stale or repeated ticket cannot complete a replacement
wait. Tokens contain no TCB pointers, so process-table compaction does not change
their identity. Exhausted generations fail instead of wrapping.

The scheduler publishes the registration before marking the selected thread
blocked. Pipe, child and console channel wakeups complete the matching active
ticket. Poll stores its exact thread and ticket alongside retained open
descriptions; runtime completion claims the ticket before writing user memory
or returning a result. Releasing a poll registration cancels its ticket and
releases each description pin once. Sleep uses the selected thread's deadline
and completes its registration before modifying the saved return frame.
Exec, exit, explicit runnable reset and native teardown invalidate registrations.

All operations require the existing serialized boot-CPU execution context.
Readiness still uses bounded scans. There is no lock-free algorithm, event-source
subscription list, interrupt registration handshake, signal interruption or SMP
claim. Native user execution still admits one active user thread per process;
selecting exact ownership in waits does not implement clone, TLS switching or
the remaining multi-threaded trap/scheduler paths.

Validation must separately record pure transition tests, scheduler and actual
runtime tests with hosted memory/device backends, freestanding target compilation,
standalone guest boot, and distribution guest execution against the new exported
SDK. A hosted test with two TCBs does not establish native pthread execution.
