# Bounded pathname UNIX streams

Private Kernel SDK ABI 62 adds immediate AF_UNIX pathname stream operations to
the native ABI and direct Linux syscall bridge. Linux UAPI remains 0.7.0. The
exported profile is `bootcpu-ram-pathname-stream-owned-queue-direct-v1`.

The kernel retains all ten architecture profiles: i386, x86_64, aarch64, arm32,
rv64, rv32, loongarch64, mips, mips64 and ppc. Linux address, option and credential
codecs use static profile selection and target byte order. This codec coverage
does not imply static-musl userland execution on ten architectures. ObfuscationOS
currently supplies musl userland for x86_64, aarch64, rv64 and loongarch64.

## Namespace and permissions

A socket pathname is an ordinary VFS socket node in a RAM filesystem. Bind uses
the caller's effective identity, resolver search permissions, parent write and
search permissions, umask, and inherited setgid group. Connect checks resolver
search permissions and write permission on the socket node. Unsupported provider
socket creation is rejected before any provider create callback.

The scheduler binding owns a move-only token that retains the socket node and
pins its mount attachment. It does not repurpose the transport open description's
node field. Unlink removes the name while existing connected streams continue;
rebinding the same text creates a new node identity. Final binding cleanup cannot
remove a later binding. Final listener close leaves a stale socket node until
unlink. Connecting to that node returns connection-refused, while a missing
pathname returns no-entry. A pinned mount cannot be detached.

ObfuscationOS owns the volatile runtime namespace policy. Its boot storage module
attaches an owned RAM filesystem at `/run`, creates `/run/user`, and creates
`/run/user/1000` with mode 0700 and owner 1000. The OS consumes an explicitly
validated architecture- and mode-specific exported SDK; its build does not
compile kernel source.

## Connections and ownership

There are 32 endpoint owners and at most four queued children per listener. The
existing 16 transport buffers limit connected pairs to eight. Each connection
creates a canonical server open description before accept. The listening queue
owns that child, so the client may send payload and SCM_RIGHTS before accept and
then close its endpoint. Existing rights ownership and graph collection include
listener-to-child queue edges and child-to-queued-rights edges. Listener teardown
removes those classifications before callbacks and releases pending payload and
rights. Repeated queue and rights cycles must return ownership to baseline.

Accept reserves a descriptor slot and a queue token. User output is copied while
the reservation remains uncommitted. Refused output cancels the reservation and
leaves the same canonical child queued for a later accept. The accepted peer
address is unnamed: its family is AF_UNIX and its full reported length is two
bytes. Peer pathname output is outside this profile.

Listen and connect snapshot the actual kernel PID, effective UID and effective
GID. Later credential changes do not change those snapshots. SO_PEERCRED encodes
the peer snapshot into three target-endian 32-bit fields and rejects a PID outside
the signed 32-bit range. Legacy socketpair streams do not have these snapshots.

The marshaller retains the original canonical open description before copied
input or output and rechecks the PID and descriptor installation after callbacks.
Closing and replacing a descriptor during a copied callback cannot redirect an
operation to the replacement description.

## Limits and evidence

Operations run synchronously on the boot CPU. Empty accept and a full queue return
would-block. Blocking connection waits, abstract names, datagrams, seqpacket,
persistent socket creation, SCM_CREDENTIALS, SO_PASSCRED and legacy pathname
socketcall are unsupported. Pathnames must include a terminator and may contain
at most 107 text bytes in a 110-byte sockaddr_un.

The hosted VFS, scheduler and syscall suites exercise real implementation state,
including all ten target-endian codec profiles. The Debug kernel test point is an
ownership model. These tests do not prove native traps, address-space isolation,
hardware, SMP, shutdown or power-loss behavior.

The separate native and static-musl probes fork before creating sockets; the
client execs a new image before creating its endpoint. They check actual peer
process credentials, preaccept payload and duplicate rights, shared file offset,
and descriptor close-on-exec. Their source and packaging are preparation until
root-owned build and guest receipts qualify them. The guest driver runs each
personality twice and records its serial and offline filesystem checks separately
from hosted models.

Linux constants come from the pinned musl 1.2.5 archive recorded in
`linux-unix-pathname-source.json`. Declaration and SDK admission checks do not
execute syscalls or load code.
