# Owned scalar pipe read continuations

Native scalar read on a pipe retains its original open description when it
blocks. Each page-backed PipeReadWait stores the PID/TID, exact scheduler ticket,
original output address and bounded count, and the retained description and pipe
identities. The numeric file descriptor is diagnostic only after the initial
call. Closing or replacing that descriptor from another thread cannot redirect
the pending read, or remove its last reader endpoint pin.

The serialized boot-CPU runtime tries a nonblocking read before registration.
If the pipe is empty and the description permits blocking, it reserves an object,
retains the description and checks registration success before publishing the
identity index. No user or asynchronous pipe producer runs between this attempt
and registration on the current boot-CPU service path. This is not an SMP event
registration protocol.

Readiness resumes the saved request before syscall adapter decoding. The current
exact ticket must match, and user output is validated again before bytes are
consumed. Ready data returns a short read; a pipe without writers returns EOF.
Spurious readiness or a competing reader can leave the pipe empty, so the same
request rearms a fresh ticket. Shared O_NONBLOCK changes are observed before
rearming. Zero-byte and immediate nonblocking reads allocate no continuation.
The existing scalar transfer bound is one 4096-byte pipe buffer. Mapping checks
and checked copyout rely on the same serialized user-mapping contract as the
native marshaller; this change does not introduce transactional copyout or SMP
mapping synchronization.

Membership is removed before pin release and object destruction. Exit, explicit
process teardown and committed exec retire all affected objects; committed exec
also does this on the nonreturnable postcommit failure path. A returnable failed
exec preserves the old continuations. The runtime sweep resolves PID/TID afresh
and retires missing or stale owners without cancelling replacement wait tickets.
Process compaction therefore carries no stored PCB or TCB pointers. The 128-object
limit matches native entry-stack capacity; its complete page reserve coexists
with poll and futex object reserves. Exhaustion leaves existing owners intact.

No exported structures or Kernel SDK ABI layout change. The initial request and
native syscall frame remain available for future captured-origin restart work.
This mechanism does not enable caught pipe signal delivery, SA_RESTART, vectored
read continuations, blocking write continuations or production signal delivery.
Internal ownership cancellation uses -ECANCELED; lifecycle cancellation is not a
userspace return claim for a retired task.

## Evidence

`python3 -B scripts/test_native_pipe_read_continuation.py --arch all --sanitize
--output <new-directory>` runs the real runtime, scheduler, object and page code
under all ten selected architecture profiles with hosted clock/memory/trap
backends. It covers close/reuse with and without an ordinary duplicate, another
pipe becoming ready first, stale readiness and exact rearm, copied output/count,
competing readers, short reads, EOF, revoked write/unmapped output before data
consumption, shared NONBLOCK, cancellation, replacement wait preservation,
failed/committed exec, process/thread retirement, compaction and actual step/ABI
completion. It also fills 128 actual wait records and tests quota rejection via
a hosted control TCB, with object and pin cleanup accounting. Poll/futex pressure
regressions check coexistence of all reserved object pages.

Hosted results are not native execution, physical hardware, isolation, or SMP
evidence. SDK-only guests and standalone kernel boot qualification are separate
consumer tasks. Test-point WAIT/COMPLETE records expose retained identity,
initial request generation, current ticket generation and global live count;
guests must additionally prove returned values/data and mutation ordering.
