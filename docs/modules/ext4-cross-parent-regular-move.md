# Bounded cross-parent regular-file move

This source prototype plans image changes only. It does not provide a mounted
transaction, journal writer, replay proof, recovery classifier, SDK export,
native application execution or Files workflow readiness. The existing mkdir,
ordered free-block initialization and populated same-parent directory rename
implementations remain intact in the selected composed ABI64 source V2.

The first profile moves a singly linked ordinary regular file into an absent
name in a different parent. Both parents use one checksummed 1 KiB directory
block. Names contain 1–31 bytes; `.` and `..`, NUL and slash are rejected. The
file uses at most 16 already allocated data blocks in initialized inline extents;
an empty regular file is also supported. The selected filesystem uses the
existing bounded 32 MiB, four-group metadata profile and an independently
checked empty orphan chain. Existing destinations, hard links, allocation,
directory moves, cross-filesystem moves and external extent or xattr trees are
outside this first profile. Existing destination replacement needs a separately
qualified orphan transition and must never silently select this absent-target
planner.

## Pure planner

`Ext4RegularMovePlanner::plan` consumes an explicit update, two complete parent
directory images, the three selected inode-table views, superblock, descriptors
and the selected allocation bitmaps. It checks geometry, UUID, descriptors,
allocation, inode generations and checksums, exact directory extent ownership,
both complete dirent streams, duplicate names, dot identities and parent link
counts before constructing output. File data may alias neither parent directory,
reserved metadata nor the journal. The source has exactly one directory alias
across the two checked blocks and exactly one inode link.

The result contains up to five full before/after home-block pairs: both directory
blocks followed by the two parent inode tables and the file inode table, with
duplicate table targets coalesced only when their complete old bytes agree. A
shared table accumulates all three inode edits without dropping an earlier edit.
The source directory entry becomes an unused record; destination insertion uses
the first fitting unused record, or the first fitting live-record slack. No
block or inode is allocated or freed. Both parents receive mtime, ctime and an
incremented version; the file receives ctime and an incremented version. Inode
checksums and both directory checksums are recomputed. Atime, file mtime, inode
number, generation, mode, ownership, size, extents and all link counts survive.
Unrelated inode slots and records survive exactly. Names in the result use owned
bounded arrays instead of retaining borrowed caller string views.

The planner has no device or write/flush callback. Normal refusals clear the
output. An output overlapping any input, including borrowed name storage, is
rejected before reset so that invalid aliasing cannot mutate an input view.
Structural and allocation inspection follows the existing namespace planner;
it is bounded admission rather than an assertion that a whole filesystem has
passed fsck. The prototype copies checked helpers into its own static planner;
it neither bypasses private C++ access checks nor broadens an existing replay
family. Future consolidation must retain the original tests and proofs.

## Required journal and replay implementation

The next implementation must introduce a distinct regular-move witness and
transaction type. `namespace_regular` replacement/unlink and create/allocation
records are insufficient. The witness must retain the two parent identities,
three generations, source identity, both names, selected event time, exact
canonical target order, every full old home image and every canonical after
image. Before the first WRITE or FLUSH, an owning transaction must validate the
complete proof and reread all home and ancillary preimages from the same
exclusive device view. A stale byte in either directory, any selected inode
table, a bitmap, superblock, descriptor or admitted journal head refuses the
operation with zero writes and flushes.

Journal construction must check an admitted empty head, filesystem and journal
UUIDs, bounded sequence, complete tags and payload CRCs, commit/end records,
padding and exact witness marker. A distinct marker must dispatch to one strict
regular-move verifier. Neither marker absence nor verifier refusal may fall back
to create, allocation, orphan or ordinary namespace replay. The verifier must
reconstruct the entire old view, recompute this planner and compare all target
and record bytes to canonical output before the first replay WRITE or FLUSH.
Observed home blocks must equal their authenticated full before or after images;
unrelated changes and torn states are refused. The proof must preserve source
data ownership without accepting provisional allocation bitmap exceptions.

The explicit crash-cut suite must cover head publication, descriptor, every
payload, commit, every home checkpoint, journal clearing and filesystem recovery
flag clearing, plus failures before each corresponding flush. Cold replay must
finish with both directory checksums and all selected inode checksums valid,
source inode/generation/data unchanged, old name absent and new name bound to
the original source identity. A planned block image or metadata parser result
is not evidence for any of these properties.

## Required mounted and native integration

The mounted operation must hold serialization and stable device identity across
planning, preimage comparison, journal ownership, completion and publication.
It must use separately measured, bounded buffers and noncopyable authority for
two directories and up to three coalesced inode-table blocks. Failed attempts
must retire their device/session ownership and leave the existing node visible
under its old parent and name. Parent/name cache publication occurs only after
qualified completion, without changing the source persistent inode identity.
The new parent/name storage must be owned and retained, and the old lookup must
be invalidated while existing handles retain the moved file identity. A record
in a process or node table is not mounted durability or process isolation.

Native Files admission must bind the exact producer SDK, SDK-only consumer,
application ELF, boot link, owned disk and retired QEMU process tree. Its missing
healthy actions begin at mkdir; the already observed `ls /home` and original
Notes `stat` prefix and original English document must remain retained without
replay. Cross-parent `mv` must pair its response with destination inode/data
checks and old-name absence. A later populated-directory rename must preserve
the moved child identity. All nine cold actions must use the same owned disk
only after healthy process retirement. No readiness claim precedes the complete
transaction/replay/mounted/native evidence chain.

## Source-only model coverage

The authored model source covers three table-sharing layouts, exact preimages,
merged time/version updates, checksum validity, source data and identity
retention, unchanged unrelated inode bytes, unused-record and slack insertion,
owned name storage, and unchanged source/ancillary images. Refusal source covers
checksum faults, stale generations, invalid names, same-parent misuse, existing
destinations, malformed and duplicate dirents, unallocated inode/directory,
source hard links, file-to-directory data alias, capacity exhaustion, all three
version-overflow positions and input/output aliasing. These sources have not
been compiled or executed. Root must qualify them with explicit compiler
closures and model observations before later transaction work is admitted.

All ten Kernel architecture profiles, twenty architecture/mode rows and four
supplied musl profiles remain required. This prototype changes no SDK ABI or
exported service profile and contains no OS consumer fallback to Kernel source.
