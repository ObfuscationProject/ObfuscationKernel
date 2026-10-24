# Bounded next initialized data block

SDK48 advertises the separate C++ profile
`bounded-mounted-dirty-aligned-inline-next-block-ordered-home-v1`.
The syscall UAPI remains 0.7.0. This private source is under qualification;
the profile declaration does not imply accepted SDK promotion or M4 completion.

The mounted-dirty writer admits one request wholly within the next 1 KiB block
of an allocated, single-link regular file with an exactly aligned 1-15-block
prefix and at most four initialized inline extent records. It initializes
the complete new block, preserves every old data block and inode identity,
extends/coalesces only the last extent, and increments the full inode version.
The checked 128-record ownership scan includes allocated reserved records and
proves unique ownership of every old block plus an unreferenced free candidate.
Unsupported mapping, capacity or ownership is rejected before write/FLUSH.

The canonical owner seals journal, four metadata homes and ordered data.
It writes and flushes the sealed data participant before metadata publication,
then checkpoints allocation homes and flushes before the inode home and its
flush. An uncertain device result quarantines the owner and mount.

Recovery has separate first-data and next-data policies. Mount selection uses
their exact union for the four-action transaction shape, with no legacy
fallback after recognition. When the inode home is old, its actual old mapping,
mtime and ctime feed the pure planner. When the inode home is after, every
allocation home must be after and every raw home must equal its payload. A
checked final overlay requires at least one strict candidate and a separate
bounded global ownership scan. It cannot identify a unique historical target
when valid siblings share the inode table. In this state completion flushes the
already checked homes, writes only the empty journal and flushes it; mounted
retain-dirty completion performs one write and two FLUSHes. It never rewrites
homes or synthesizes an old timestamp. The eight old-inode allocation mixtures
and all-after cleanup are separate test cases. Final-state validation does not
prove every potential candidate's prior writer history.

Hosted tests, sanitizers, freestanding object checks, boot-reached negative
models, actual Virtio service execution and UID1000 ELF integration are distinct
evidence. No physical power-loss or SMP claim follows from a hosted or QEMU run.
Arbitrary multiblock requests, extent trees, replacement rename, cleanup unlink,
directory growth and complete Notes atomic replacement save remain outside
this slice. Physical/manual acceptance remains separately deferred.
