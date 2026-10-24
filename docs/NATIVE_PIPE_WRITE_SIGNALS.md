# Owned scalar pipe writes and private caught signals

This in-progress successor depends on frozen Kernel827. It has not been
promoted or qualified for native execution. All ten architecture profiles and
the existing SDK29 records remain required. Production signals stay disabled.

A blocking scalar write owns its PID/TID, numeric input pointer, bounded count,
original open description, pipe identity, request generation and current exact
wait ticket. A shared descriptor table can change while the operation blocks;
the continuation does not resolve that descriptor number again. The object pool
is bounded by entry-stack leases and included in the object-page reservation.

The initial attempt and each readiness retry validate and copy the input bytes
before publishing them to the pipe. The request is copied, not the payload:
a peer can change input memory while the caller blocks. The resumed operation
observes the current mapping and bytes. This preserves the existing bootstrap
early range admission; it does not claim all Linux copy/error precedence.

Small requests retain the scheduler's 4096-byte PIPE_BUF atomicity. A request
larger than PIPE_BUF remains distinguishable by the 4097-byte staging bound.
An immediate short write completes normally without acquiring an interruptible
wait; this change does not loop a large blocking write until its entire original
length is transferred. Waking without sufficient space rearms the same request
with a new ticket. An old object cannot cancel a replacement wait.

Private caught signals can interrupt only an owned zero-progress blocked write.
Available space sufficient for progress and the no-reader error take precedence
over caught delivery. EINTR publishes the normal return continuation; SA_RESTART
publishes the original validated syscall instruction and arguments, then retires
the old request. The eventual re-entry obtains a fresh wait and resolves its
descriptor anew. N64/PPC use the typed restart publication path. Ignored, blocked
and stale-origin signals cannot acquire publication authority.

Thread exit, group finish, process release, exec retirement and stale-wait
pruning cancel the owned operation and release the description pin. Separate
PIPE_WRITE diagnostics identify admission, rearm, origin and terminal events.
Consumer integration and full native validation are still required.

This slice does not implement writev/pwrite pipe continuations, automatic
SIGPIPE generation, full blocking large-write completion, SMP synchronization,
POSIX certification or hardware qualification. Hosted runtime tests with mock
trap/copy backends are recorded separately from real user execution.

The complete scalar/vector blocking-write successor supersedes the bounded
short-write restrictions above; see `NATIVE_PIPE_BLOCKING_WRITES.md`.
