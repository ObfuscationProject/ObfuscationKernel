# Linux descriptor bootstrap

The selected Linux adapters implement dup, dup2 where present, dup3, and
fcntl/fcntl64 DUPFD, DUPFD_CLOEXEC, GETFD, SETFD, GETFL and SETFL for all ten
profiles. Numbers and source hashes are generated from pinned musl 1.2.5 and
Linux 6.13.4. Descriptor/command/flag/minimum scalars use their Linux 32-bit
width even on LP64; MIPS and PowerPC retain their native Linux return convention.

All duplicates share the canonical open description and file offset. APPEND
and NONBLOCK belong to that description; CLOEXEC belongs to each descriptor.
The common duplicate operation installs CLOEXEC with the new descriptor before
returning. Replacement retains the source before closing the previous target;
invalid source/target/flags leave existing references intact. dup2 of the same
valid descriptor preserves its flags, while dup3 rejects equal descriptors.

GETFL converts access/status bits back to the selected Linux ABI, excludes
creation-time and descriptor flags, and preserves the large-file admission
policy of the original open. The added description field and duplicate method
signature require Kernel SDK ABI 10; consumers must reject older generations.
Native UAPI remains 0.4.0. This does not add complete 32-bit off64 semantics.

SETFL supports APPEND and NONBLOCK, ignores immutable/unknown bits according to
the Linux scalar path, and rejects selected mutable DIRECT/NOATIME/ASYNC flags
before changing state. Locking, async notifications, leases, seals and other
commands remain unsupported (EINVAL), and invalid descriptors return EBADF.
The descriptor table remains bounded at 32; DUPFD minima outside that table
return EINVAL, dup targets outside it EBADF, and allocation pressure EMFILE.

The current execution path serializes operations on the boot CPU. This is not
SMP fd-table locking, descriptor-table expansion or full POSIX fcntl support.
Hosted tests use the real common service and adapter with mocked user mappings;
QEMU musl tests separately exercise shared offsets/status, repeated fork/exec
inheritance, CLOEXEC closure, exhaustion/reuse, and persisted file contents.

Reproduce hosted adapter checks with `scripts/test_linux_fds.py --sanitize`;
`scripts/test_linux_bootstrap.py` cross-compiles all ten adapters. OS probes use
`tests/musl/fds.c` and `scripts/test_musl_userland.py --require-fds`, which also
requires preceding file, heap, process and pathname acceptance and offline
ext4 bytes. An available runner is not a successful execution receipt.
