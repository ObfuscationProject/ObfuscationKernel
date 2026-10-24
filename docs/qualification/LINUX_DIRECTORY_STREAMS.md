# Bounded Linux directory streams

The Linux personality adapts getdents64 to the retained common open description.
The current union requires Kernel SDK/module17 and native UAPI0.5.0.
Directory behavior originated in the isolated SDK15/UAPI0.4.0 candidate.
All ten profiles require fresh matching producer/SDK-only consumer artifacts;
earlier component candidates are not interchangeable with this union.

The constrained `enumerate_directory<DirectoryPolicy>` callback publishes each
complete record before advancing the shared description cursor. Native output
retains its prior index-based inode and no-dot format. Linux output uses qualified
provider inode identities, exactly one `.` and `..`, target byte order and eight
byte alignment, including 32-bit profiles. Padding is zero. RAM hard links retain
their shared inode; ext4 listings preserve the actual directory-entry inode.
No address or listing index substitutes for an unavailable Linux inode.

Checked metadata, listing and parent lookup errors propagate. An invalid listing
count is rejected before indexing. Names containing slash or NUL, empty names,
duplicate provider dots and unavailable inode identities never become successful
records. Prior completed records can be returned before a later failure; retry
starts at that same uncommitted entry. EOF requires a successful provider read.

The fd and count are Linux unsigned-int scalars. The adapter validates descriptor
and directory before copying output. A nonempty first record with zero, too-small
or greater-than-INT_MAX count returns EINVAL; an inaccessible first record returns
EFAULT without advancing the cursor. EOF returns zero even for an inaccessible
output. Requests publish at most 4096 bytes. A later copy fault returns the prior
complete bytes and preserves the next record position. Bytes partially written
within a failed record are not rolled back; exact Linux within-record side effects
and concurrent map revocation are outside this qualification.

Cookies are logical positions in the bounded listing, stable only while the
namespace is unchanged. dup/fork share them; independent opens do not. SEEK_SET
and SEEK_CUR retain existing signed native-word bounds. Linux directory SEEK_END
returns EINVAL without changing position because directory byte size is not a
cookie endpoint. Native seek behavior remains unchanged. No equality with Linux
ext4 hash cookies is promised. Providers retain 64-child and 32-byte-name limits.

Calls are serialized on the boot CPU. The consumer callback must not yield,
reenter or close descriptions; it only encodes and copies a record. This is not
SMP or concurrent namespace-mutation qualification. Archive providers lacking
identity return ENOSYS. Linux stat-family calls and therefore musl fdopendir
remain unavailable; ordinary opendir/readdir/closedir/telldir/seekdir/rewinddir
are the exercised public interfaces.

The generated ABI source inventory pins Linux 6.13.4 readdir semantics and musl
1.2.5 directory wrappers/headers. Hosted adapter tests cover all ten byte orders,
independent golden records, exact guards, shared cursors, provider failures and
native regression; these do not execute guest hardware. The native cumulative
musl probe emits the real inode of `musl-positional`; the OS runner verifies two
identical receipts and compares them against offline debugfs on the actual test
image. An exact generation-14 kernel passes preceding gates then fails the first
readdir with ENOSYS. QEMU evidence remains separate from physical machines.
