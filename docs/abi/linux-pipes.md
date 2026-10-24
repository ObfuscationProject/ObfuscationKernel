# Linux pipe adaptation

Admit Linux pipe/pipe2 on all ten retained Linux profiles, using Linux 6.13.4
number/flag definitions and musl 1.2.5 call conventions. MIPS O32 and N64 pipe
returns its descriptors in v0/v1 with the existing a3 error indication; it has
no user pointer. Every other present pipe syscall and every pipe2 syscall
uses an eight-byte target-endian array of two 32-bit descriptors.

Pipe allocation, endpoint state, blocking, EOF, descriptor duplication, fork
references, close-on-exec and teardown remain owned by native POSIX/scheduler
services. The adapter checks flags, reserves resources, then copies the target
pair through the checked user boundary. Copy failure closes both endpoints
before returning. There is no intervening scheduling point or userspace observer.
Linux int flags are normalized to 32 bits.

Tests distinguish hosted copy-fault/ownership/ABI models from actual target
execution. The new musl probe covers pipe/pipe2 creation, byte exchange, empty
nonblocking EAGAIN, EOF after the final writer reference, dup lifetime and fork
exchange. Native pipe tests remain and gain copy-failure rollback coverage.
All ten targets compile; independent exact SDK consumers execute all ten QEMU
profiles. Existing file/heap/process/path/descriptor checks remain intact.

No signals or SIGPIPE delivery are implemented here. The current native
readerless-write error contract will be measured and reported explicitly;
Linux-specific errno translation will not imply asynchronous signal support.
No new scheduler resources, pipe implementation or capacity is introduced.
Adding Operation entries changes the exported ProfileData layout. The combined
pipe/seek integration uses SDK ABI 11, and the OS consumer rejects ABI 10 and
earlier artifacts. Rebuild every architecture/mode export and native extension.
The earlier isolated pipe/seek candidates used an inherited ABI 10 label solely
for their local qualification; that label did not establish ABI 10 compatibility.

Admission preserves Linux error precedence: invalid flags first, global pipe/
open-description capacity (ENFILE), per-process fd capacity (EMFILE), then checked
copyout (EFAULT). Common native allocation reports distinct capacity statuses
without publishing any state on admission failure. Hosted combined-failure
cases and an actual parent/child global-pressure handshake exercise this order.
Packet-mode O_DIRECT and notification-pipe O_EXCL remain rejected. Native
readerless nonempty write returns EPIPE (32), empty write succeeds, and no
SIGPIPE signal is sent. Hosted and guest tests explicitly cover this errno-only
behavior.
