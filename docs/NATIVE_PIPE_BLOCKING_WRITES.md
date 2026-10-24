# Complete serialized blocking pipe writes

Scalar pipe `write` and pipe `writev` retain the full admitted request. The scalar
length and vector sum must fit SSIZE_MAX; overflowing address arithmetic is
rejected. Up to 16 copied vector entries belong to the continuation. Their
metadata is never reread after admission. The original open description is pinned
across waits, including numeric descriptor close/reuse or replacement.

Each owned request records its full byte count and a monotonic committed cursor.
Every dispatch stages at most PIPE_BUF bytes. For an originally large request,
the staged amount is exactly the smaller of remaining bytes and available pipe
space. Successful nonterminal publication therefore fills the pipe before a new
wait is registered. The runtime yields after that registration. There is no
busy-loop, FIFO writer fairness promise or SMP synchronization claim.

`SchedulerState::write_pipe` preserves its existing atomicity for all callers.
The additional `write_pipe_fragment` interface receives the original request's
atomicity. An originally small write remains all-or-nothing; an originally large
write can publish a partial final suffix even when that suffix is below PIPE_BUF.
The new function changes the exported SDK interface, not public record layouts.

Payload bytes are copied immediately before publication. Small requests preserve
their eager validation and fault precedence. Large requests first observe readers
and capacity, then check/copy only the unpublished fragment. A consumed-prefix
unmap does not invalidate the remaining suffix. Copying several vector spans is
transactional with respect to the pipe: a fault discards that staged fragment.
This is bounded fragment fault granularity, not an emulation of Linux pipe-page
merging or every fault precedence. A peer's payload edits are observed by a later
attempt; metadata edits are not.

Blocking requests return their full count unless interrupted by a terminal event.
After progress, EFAULT, allocation or wait-generation exhaustion, O_NONBLOCK
EAGAIN, and an exactly owned external cancellation return the positive committed
count. Before progress they return the corresponding error. Changing O_NONBLOCK
on the retained description is observed at the next readiness retry; setting the
flag itself does not synthesize a wake. Thread/process retirement and exec release
the pin and object without publishing into a replacement syscall or image. No
terminal condition rolls back bytes already available to readers.

A request that acquired a genuine syscall origin retains its admission capture
identity separately from the live origin record. Both ready and cancelled
continuations reject a replaced frame/image or a revoked capture before further
publication. An origin revoked after admission cannot become an originless call.
Bootstrap calls that never acquired restart capture retain that distinct behavior.

The internal origin binding includes committed bytes. Progress updates only that
cursor on the same ticket; rearm changes only the ticket. Completion ownership
remains valid, but every positive cursor rejects restart permits. Old zero-progress
permits fail after publication. A caught signal therefore returns the positive
prefix at the normal syscall continuation even with SA_RESTART. With zero progress,
the existing EINTR/restart behavior applies. Sigreturn retains its existing support
for intentional user edits to the completed result.

Reader loss generates automatic caller-directed SIGPIPE through the existing
private Debug signal service, including after progress. The syscall returns EPIPE
only if no bytes were committed. Otherwise it returns the positive prefix. Wait,
pin and restart authority are retired before queuing the terminal signal. Production
signals remain disabled; ordinary builds do not acquire private signal delivery.

Requests of at most PIPE_BUF retain their previous diagnostic formats. Larger
requests use `OK_NATIVE_PIPE_WRITE_LARGE_{WAIT,ORIGIN,COMPLETE,SIGNAL}` or the
corresponding `WRITEV_LARGE` names. Their `bytes` field is the complete request;
`committed` is the published prefix at that record. WAIT `stage` is `arm` or
`rearm`, and the request ID remains the first ticket generation. ORIGIN is emitted
for a valid completion binding even if progress prevents restart. SIGNAL uses
`outcome=partial` after progress, otherwise the existing `restart` or `eintr`.
COMPLETE keeps its cause (`ready`, `efault`, `error`, `cancelled`, `interrupted`)
and records the actual positive or negative terminal result. Terminal generated
SIGPIPE is represented by COMPLETE; it does not create an interrupted SIGNAL
record. A native consumer must explicitly implement these new namespaces.

The hosted test runs the actual dispatcher, scheduler, wait/object pools, origin
tracker, signal service and x86 frame/sigreturn with mock hardware. It distinguishes
hosted assertions from freestanding ten-target compilation and native execution.
Useful inherited SIGPIPE, scalar signal and vector continuation tests are retained;
former immediate large short-write expectations now explicitly use O_NONBLOCK or
exercise the full blocking continuation. SDK and guest qualification are separate.
