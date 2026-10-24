# Bounded Linux poll and null-mask ppoll

This slice adapts Linux `poll`, `ppoll` and `ppoll_time64` to the existing
serialized boot-CPU descriptor wait mechanism. All ten architecture profiles
remain present. Linux 6.13.4 `fs/select.c`, `fs/pipe.c`, `kernel/signal.c`, poll
UAPI headers and musl 1.2.5 `src/select/{poll,ppoll}.c` are pinned by the ABI
generator. The source manifest distinguishes local Linux source identity from
upstream archive authenticity.

`pollfd` is an eight-byte target-endian record: signed 32-bit fd, 16-bit events,
16-bit revents. Only revents is copied back. Linux's unsigned 32-bit nfds and
signed 32-bit millisecond timeout are narrowed even on LP64. The current
per-process limit is 32 records. Negative fds are ignored, invalid fds return
POLLNVAL, and ERR/HUP are independent of requested bits. Normal read/write aliases
are translated explicitly, including MIPS POLLWRNORM == POLLOUT.

The common readiness implementation statically selects native or Linux pipe
semantics. Native EOF readability and peer-error behavior are preserved. Linux
empty EOF returns HUP without synthetic IN/RDNORM; a pipe write endpoint with
capacity but no readers returns requested OUT/WRNORM together with ERR. The
qualified providers remain regular files/directories, native console and pipes.
Priority/band/socket readiness and epoll are not implemented. Bounded select/pselect
uses the same wait engine; its set semantics are documented in linux-select.md.

Both syscall boundaries copy descriptors into a typed `PollRequest` before common
admission. The wait record retains each open description, including duplicate
entries, and stores output encoding and timeout-copy metadata. Subsequent user
buffer mutation or fd reuse cannot redirect that wait. Completion is performed
once by the runtime with the actual process personality; it does not redispatch
the syscall or reload its timeout. Exec/exit retirement releases wait pins.
The native boundary retains its initial whole-output-range preflight. Linux
checks readability on entry and writes only revents fields at completion, so a
readonly input field does not reject an otherwise writable revents field.

`poll` treats a negative millisecond timeout as infinite. A null ppoll timeout is
infinite; a non-null timeout is validated before the signal-mask stage and before
nfds/descriptor input. Legacy two-long timespecs and sixteen-byte kernel time64
records use the selected target's layout. As Linux specifies, 32-bit time64 input
ignores high tv_nsec padding. Positive timeouts become one monotonic deadline;
the wait clock saturates at KTIME_MAX. The separate raw timeout endpoint uses
Linux's safe timespec addition and TIME64_MAX saturation, preserving large
remaining intervals instead of narrowing them to the wait clock's capacity.

Raw ppoll copies the remaining interval back after readiness, timeout or errors
from descriptor admission/copyout, except for an original zero interval. A failed
timeout copyout is ignored and never replaces that result. Invalid initial time
or mask admission returns before this copyback stage. Musl uses a temporary raw
timespec, so its public ppoll leaves the application's timeout unchanged.

Only null signal masks are admitted. A null mask ignores sigsetsize. For a
non-null mask, size validation (16 bytes for MIPS, 8 elsewhere) precedes checked
mask input; valid readable masks then return ENOSYS without a wait or timeout
copyback. This deliberate boundary does not emulate atomic mask installation,
signal interruption, EINTR/restart, libc cancellation, pthreads or signal delivery.

The new generated Operation entries change exported ProfileData arrays. The new
typed poll request/completion metadata and readiness policy API also require
matching producer and consumer source artifacts. This candidate therefore uses
Kernel SDK ABI 14 after the select extension and rejects older artifacts; labels are not compatibility
shims.

Hosted adapter tests cover all ten layouts and syscall conventions, error order,
raw timeout updates, retained description lifetime, copy faults and actual runtime
completion through a hosted execution backend. Cross compilation is separate
from target execution. Cumulative musl probes preserve prior file, heap, process,
path, descriptor, seek, pipe and sleep suites and add real poll/ppoll timeout and
two-process pipe wake checks. Evidence remains scoped to QEMU execution on the
boot CPU; it establishes neither hardware SMP nor physical-device behavior.
