# Checked ext4 read-only replay preview

`Ext4ReadOnlyView` in `ok/fs/ext4/layout.hpp` supplies static-dispatch decoding
over a filesystem-block reader. `Ext4ReplayPreview` in `ok/fs/ext4/txn.hpp` binds
an actual device through `Ext4RecoveryBinding`, plans committed JBD2 actions with
revoke filtering, and provides a final read-only overlay. This is inspection: it
does not write home blocks, flush, clear recovery flags, advance the journal tail,
publish a VFS mount or grant replay execution. Existing ext4 mount admission
remains unchanged.

## Lifetime and access

Both objects are one-shot and noncopyable. The owner must hold exclusive access
to the underlying device and exclude replacement, caches and other writers for
the entire inspection lifetime. Geometry is checked around every device read;
geometry checks are not a device/cache ownership mechanism. The standalone view
requires prior validated `Ext4RecoveryInfo`; the preview derives its own binding.

`open()` returns success only after binding, journal planning, final superblock,
group/root checks, and validation of every retained journal action. For a target
with multiple committed actions, final reads select its last nonrevoked payload;
policy validation compares each retained action to its preceding version. Payload
CRC and escape encoding are checked each time journal bytes are consumed.
A later committed revoke prevents old payloads from appearing in the final view;
uncommitted tails are excluded. Action/revoke storage is caller-selected through
template capacities and capacity exhaustion rejects the attempt without partial
publication. Reports retain the stage, error and plan/validation counts.

`inode`, `read`, streaming `directory`, and single-component `lookup` are available
after successful admission. Directory names are borrowed only during the visitor.
Directory scanning validates record bounds, names and checksums; it does not
perform a full-volume namespace/allocation fsck or eagerly decode every child.
Lookup additionally validates the selected inode and recorded directory type;
a found-but-unallocated inode is corruption, whereas a name absent from the
directory is an ordinary miss. A direct request for an unallocated inode reports
not-found. I/O/checksum/corruption failures retire inspection. An unsupported
inode representation can leave unrelated inspection available. Output buffers or
visitor side effects may be partial on failure; callers must discard them.
No path traversal, permission enforcement, directory-tree cycle detection or
symlink following is performed.

## Declared bounded representation

The binding's exact Linux ext4 feature gate is preserved: internal contiguous
journal, synchronous JBD2 CRC32C v3, 1/2/4 KiB blocks over 512-byte sectors,
32-bit filesystem addresses and sizes, up to 32 groups, 256-byte Linux inodes with
32 bytes of extra fields, and UUID-seeded metadata checksums. Structural metadata
locations, inode/block allocation checksums and journal exclusion are checked.

Ordinary files and directories require inline extent roots of depth zero, at
most four physical runs, nonoverlapping extents and exact sector accounting.
Sparse and unwritten file ranges read as zeros. Fast symlinks and one-block
symlinks in canonical direct-pointer or one-initialized-extent form are decoded.
Directories use checksum tails, names up to 255 bytes, and a 1024-block scan
budget. External extent trees, indexed directories, xattrs/ACLs, special inodes,
large files, and other modern format combinations are not admitted by this
reader. Raw file contents themselves have no checksum.

## Allowed replay-preview changes

The preview permits only ordinary regular-inode mode/uid/gid, ctime and version
updates, their checksum fields, and these primary-superblock byte intervals:
`[44,54)`, `[136,200)`, `[376,384)`, `[1020,1024)`. These encode mount bookkeeping
and its checksum. Size, allocation, links, mapping, namespace, filesystem/journal
identity and other structural changes reject the preview. Every changed inode's
before and after checksums must both be valid; torn home-inode repair is outside
this policy. This deliberately bounded whitelist does not authorize execution
of arbitrary Linux recovery transactions or establish writable interoperability.

## Validation

`scripts/test_ext4_replay_view.py --sanitize --cross --output PATH` snapshots its
sources, creates private e2fsprogs 1/2/4 KiB fixtures, records readonly fsck and
inode references, runs failure/corruption models, and checks actual cross-object
machine/class/endianness for all ten architecture profiles. An explicit
`--linux-dirty-image` adds the separately qualified actual Linux transaction.
Hosted byte-array faults are models, and cross-object compilation is not CPU
execution. All writes and flushes are rejected and counted in those models.

The OS owns `tools/jbd2/test_preview.py` and its native external-driver probe. It
requires an exact exported SDK, admitted Linux fixture and explicit toolchain;
it never compiles Kernel sources. It boots private readonly Virtio disks for
metadata-preview, checksum-valid mapping rejection and corrupt-payload rejection,
records complete positive markers, checks whole-disk immutability, and revalidates
its frozen SDK. Native QEMU execution does not establish physical power-loss
behavior. Read-only preview remains separate from recovery, read-only mounting,
writable mounting, journal writing and recovery completion.
