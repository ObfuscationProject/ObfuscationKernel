# Private positive-target process signals

The native signal runtime implements positive signed PID `kill` and one stable
pending queue per process. The original qualification was private Debug; the
ordinary successor is selected by `OK_NATIVE_SIGNAL_RUNTIME` in Debug/Release,
with private evidence traces separately gated. See `NATIVE_SIGNAL_PRODUCTION.md`.
All ten kernel profiles remain. Historical SDK29 evidence below is not relabelled
as ordinary SDK31 execution.

## Routing and admission

Positive targets are looked up in the global task-ID namespace. A live nonleader
TID names its owner group; an exited nonleader is absent. A retired leader still
names living siblings. Lookup precedes signal validation; permission follows
validation, using the existing same-group, effective-root and real/effective
sender versus real/saved recipient UID rules. The target's effective UID alone
does not confer permission. Zero is a side-effect-free permission probe. Retained
zombies accept valid probes/sends without allocating or reviving signal state.

Successful generation retains `SI_USER` with the sender group PID and real UID.
Ignored generation examines the named task's current blocked mask. An ignored
unblocked named target drops the signal, even if a sibling blocks it. A blocked
named target retains it. For ppoll/pselect the active temporary delivery mask is
used; its saved return mask is not Linux's `real_blocked` sigtimedwait state.

Each recipient uses one typed borrowed view: eligible directed work precedes
eligible shared work, with existing priority/coalescing/FIFO inside each queue.
The same view is used by handler delivery, architecture readiness gates, sleep,
pipe read/write including vectors and partial progress, child wait, futex and
poll/select. Shared work remains in its process queue until consumption; it is
not assigned to a thread at send time. `rt_sigpending` returns the union of the
caller-directed and group-shared masks, intersected with the caller's blocked
mask. Disposition-to-ignore drains both before old-action copyout, including
when that copyout fails.

All-masked sends succeed and retain work. For ordinary nonignored signals,
unmasked tasks must include a runnable task or a qualified existing wait owner;
otherwise the private runtime returns `EOPNOTSUPP`. Caught console reads are
not yet qualified. SIGKILL instead records a group-fatal request and services it
at the next serialized step before clock/deadline/wait completion or user entry,
including console waits. Self-kill does not retire its syscall frame while that
frame is still publishing its return.

## Stable lifetime and pressure

The 64 existing stable action slots each own a nonmovable shared queue and a
nonwrapping epoch. Shared identity is `{pid, pid, epoch}`: its second key is a
group owner tag, not a selected recipient's entry-stack lease. Group retirement
drains/deactivates shared state. Fork starts empty; thread clone shares the group
queue; exec preserves it while retaining only the surviving caller's directed
work. Removing or compacting other PCBs cannot move it.

Directed retirement clears its queue and alternate stack and resets queue owner
identity to an impossible-task tombstone. This invalidates old tickets and
borrowed recipient views without reusing sequence numbers. Exec retires departed
thread owners after the caller's directed transfer; the retained leader lease
keeps its valid owner. Shared pending survives the departure of a selected peer
and remains available to siblings.

Shared queued records use the existing 31 standard slots and 32 realtime slots.
When realtime allocation fails, kill succeeds without adding an occurrence if
that number is already pending. Otherwise a bounded per-number fallback ticket
retains one zero-sender `SI_USER` occurrence. If a later allocation for the same
number succeeds, its payload subsumes the fallback; it is not an extra event
behind the allocated list. Other-number payloads leave it intact. Directed
tkill/tgkill retain their existing queue-full errno behavior.

Fallback tickets validate queue address, owner, nonreused sequence and payload.
Discard, reset, replacement and retirement revoke them. Slot epoch exhaustion
refuses admission. Exhausted sequence authority cannot silently lose an accepted
signal: an exhausted standard enqueue or fallback sequence sets a terminal
native-service fault. This remains a bounded serialized boot-CPU implementation,
not an SMP queue or production resource policy.

## Explicit boundaries and validation

Nonpositive process-group/broadcast targets, stop/continue job control, sessions,
capability namespaces, new fault/timer payloads, sigqueue value delivery,
sigwait/sigsuspend and production enablement remain separate work. Existing CPU
context, frame, trampoline and restart admission rules remain in force.

`scripts/test_native_process_signal.py` exercises actual hosted runtime routing,
permissions, masks, handler payloads, fork/nonleader exec, retirement, queue
pressure, stale authority and group kill. `test_native_process_signal_waits.py`
replays retained wait assertions with shared enqueue, including real runtime
publication and cleanup. These runs use mocked traps/mappings/usercopy and must
not be reported as native guest or physical execution. Separate author mutation
controls, ten-profile ordinary/private objects and public-layout comparisons
precede root-owned SDK/native/OS integration qualification.

Automatic typed SIGCHLD is now supplied by the separate opt-in
[child-exit integration](NATIVE_CHILD_EXIT_SIGNALS.md). Its special explicit-ignore
suppression does not change user-generated kill(SIGCHLD) admission.
