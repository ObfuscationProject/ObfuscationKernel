# Private automatic pipe SIGPIPE

The existing private Debug signal runtime now generates thread-directed SIGPIPE
when a scalar write or writev observes that its retained pipe has no readers.
This includes reader loss after a blocking write has committed a positive prefix. Production caught signals remain disabled. All ten architecture
profiles and the existing public record layouts are retained.

Generation targets the calling thread's generation-owned queue. Linux pipe_write
uses SI_USER (0), the current thread-group PID and real UID for this signal; it
does not use SI_TKILL or zero-payload SI_KERNEL. The existing directed enqueue
service discards an ignored, unblocked signal and retains an ignored, blocked
signal. Blocked caught/default signals remain pending. A previously pending
standard SIGPIPE keeps its earlier payload and delivery ticket. Unrelated
pending signals are preserved and ordinary signal selection determines order.

The terminal result is EPIPE with zero progress or the positive committed count. On resume, the old object, pinned description and
restart authority are retired before the signal is queued. Caught delivery sees
that terminal result and the normal syscall return PC in its ucontext, including SA_RESTART;
sigreturn does not replay the failed write. Unblocked default disposition
terminates through the existing signal-delivery path before another user entry.
Masking delays delivery; ignored disposition allows the terminal result to be observed normally.

Zero-byte success, invalid descriptors, eager mapping failures, non-pipe I/O,
nonblocking EAGAIN, cancellation and successful writes with live readers do not
generate SIGPIPE. This retains bootstrap eager range-check precedence rather than all
Linux fault ordering. A missing live caller/queue or exhausted pending identity
is an internal runtime failure and prevents another user entry rather than
silently omitting a required signal. No process-wide signal target selection is
introduced.

`test_native_pipe_sigpipe.py` executes the actual runtime dispatcher, wait pools,
private pending/delivery service and x86 signal frame/sigreturn implementation
with hosted mock hardware. The ordinary mode verifies that production behavior
remains EPIPE without automatic private delivery. Ten-profile freestanding object
builds are compilation evidence; Linux reference comparisons are Linux evidence.
Neither substitutes for native automatic-SIGPIPE guest qualification. SMP,
physical hardware and production signals remain gates. Complete blocking-write
progress is documented in `NATIVE_PIPE_BLOCKING_WRITES.md`.
