# Native waits and descriptor readiness

The native boot-CPU runtime supports `CLOCK_MONOTONIC`, `nanosleep`, monotonic
`clock_nanosleep`, `poll`, `pipe2` and shared `O_NONBLOCK` status flags. These are
bounded native mechanisms, not complete POSIX wait, signal or SMP semantics.
The platform clock sources and calibration restrictions are documented in
[platform clocks](PLATFORM_CLOCKS.md).

## Ownership and completion

Sleep copies its time64 request once and stores an absolute deadline with a
registration generation. A timeout completes the saved syscall directly; it does
not re-read user memory or restart the relative interval. Exec/exit cancels the
registration. A successful sleep leaves the remainder untouched; absolute sleeps
ignore it. Wall-clock time, signal interruption and restart/cancellation remain
unimplemented.

Poll accepts at most 32 entries. Each valid entry retains the scheduler-owned
open description, independently of the descriptor number. The copied request,
output address and absolute timeout belong to one bounded runtime registration.
Negative descriptors are ignored; invalid nonnegative descriptors return NVAL.
Completion releases every pin once. Process exit or native teardown cancels the
registration, including its timeout and pins. Readiness is rescanned immediately
before blocking and at each subsequent runtime step; on this serialized boot-CPU
path no device callback or peer syscall can run between the scan and registration.
This scheme needs a synchronized event-registration protocol before SMP or native
multithreaded descriptor mutation is enabled.

Poll only writes `revents`, after checking the completion destination again. It
never rereads the request after blocking. Duplicate entries count separately.
Zero timeout returns immediately; negative timeout waits without a deadline;
zero descriptors can wait for a timeout. Readiness of ordinary files/directories
is synchronous. A pipe reports buffered bytes or EOF as readable, free capacity
with readers as writable, no writers as HUP and no readers as ERR. HUP/ERR/NVAL do
not depend on the requested event mask. Console input readiness is supplied only
by the runtime input owner; output-console readiness is immediate. Other device
classes must implement an explicit provider before poll can accept them.

The outer GUI/input service loop remains live while all user processes wait.
These waits currently use bounded scans in that loop, not an interrupt-driven
idle scheduler. They do not establish the workstation's no-idle-polling budget.
`select`, `ppoll`, `pselect`, socket events, signals, cancellation and generic wait
queues are still separate gates.

## Scalar pipe read ownership

Blocking scalar native pipe reads retain the original open description and copied
output request across descriptor close/reuse. Readiness rechecks that retained
pipe, and spurious readiness rearms an exact new ticket. See
[owned scalar pipe reads](NATIVE_PIPE_READS.md) for lifecycle, quota and evidence
contracts. This does not enable caught pipe signals or syscall restart.

## Nonblocking descriptions

`O_NONBLOCK` and `O_APPEND` are shared through open descriptions across dup/fork.
`F_SETFL` changes those mutable flags while preserving the access mode. `pipe2`
validates NONBLOCK/CLOEXEC before allocating either endpoint. Empty reads and
full writes return EAGAIN without placing the caller in a blocked state when
NONBLOCK is set. Small writes, including gathered vectored writes, preserve the
4096-byte pipe atomicity boundary. A readiness report reserves neither data nor
space: a competing consumer can still make a later nonblocking call return EAGAIN.

## Evidence layers

The opt-in private Debug signal runtime additionally interrupts owned Linux
nanosleep/clock_nanosleep deadline waits. Recipient selection rechecks its mask,
action and exact wait ticket, including when the whole PCB is blocked. The
pending captured syscall frame supplies the remainder address and ABI format;
the original request is not reread. A caught signal returns EINTR even with
SA_RESTART, relative remainder copy failure returns EFAULT, and absolute or
successful sleeps leave remainder untouched. Expiry and signal claim the same
registration once. Typed ABI completion precedes handler construction. This
private path does not enable production signal delivery, other blocked waits,
restart_syscall, cancellation or hardware SMP. Hosted models and SDK-only actual
guest qualification remain distinct.

Hosted readiness tests cover description pins across close/reuse, shared flags,
pipe peer death, aggregate atomic writes and runnable EAGAIN behavior. Deadline
and clock-conversion tests check boundary, overflow and cancellation behavior.
Native `posix-check` exercises real syscalls, elapsed monotonic time, pipe wakeups,
EOF/error readiness, copy faults and finite/infinite poll on QEMU guests. `/init`
also sleeps before creating another native process. These guest checks do not
qualify physical clocks, SMP or power management.
