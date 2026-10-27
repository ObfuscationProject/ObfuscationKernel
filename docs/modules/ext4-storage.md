# Device-backed ext4

`Ext4FileSystem` is a kernel filesystem mechanism over the runtime `BlockDevice`
service boundary. `Ext4Node` integrates lookup, listing, permissions, creation,
read/write, truncate, symbolic links, unlink/rmdir, rename, hard links and synchronization with the generic VFS mount
namespace. The OS owns Virtio transport drivers, image creation and root selection.
The old `Ext4Volume` metadata reader and RAM-backed block models remain separate.

The ext4 and JBD2 mechanisms are declared in eight layered module headers under
`include/ok/fs/ext4/`; `Ext4FileSystem` and `Ext4Node` are in `layout.hpp`.
[`include/ok/fs/ext4/README.md`](../../include/ok/fs/ext4/README.md) lists the
modules, their layering and the private partials.

## Supported on-disk format

The initial writer deliberately supports a bounded, non-journalled ext4 subset:

- 512-byte device sectors; 1, 2 or 4 KiB filesystem blocks; 128/256-byte inodes.
- At most 32 block groups, 32-bit filesystem block addresses and file sizes.
- `extent`, `filetype`, `sparse_super`, `large_file` features only.
- Extent tree reads up to depth three; writes/truncate require inline extents,
  at most four physical runs. Sparse holes read as zeros. Initialized extents
  beyond EOF and unwritten-extent mutation are explicitly unsupported.
- Regular files, symbolic links and ordinary linear directories; Linux-layout 32-bit uid/gid updates; other creator OS layouts are rejected.
- 128 cached nodes, 31-byte component names, 64-entry directory listing buffers.

Journal/JBD2 replay, metadata checksums, 64-bit group descriptors, indexed
directories, bigalloc and inline data are not implemented. Unsupported feature bits are rejected before any
write. The default modern `mke2fs -t ext4` feature set is therefore not writable
by this implementation. ObfuscationOS `tools/mkext4.py` produces the supported
format explicitly, with initialized inode tables and no journal.

## Symbolic links

The on-disk representation follows Linux 6.13.4
[`fs/ext4/namei.c`](https://github.com/gregkh/linux/blob/v6.13.4/fs/ext4/namei.c) and
[`fs/ext4/inode.c`](https://github.com/gregkh/linux/blob/v6.13.4/fs/ext4/inode.c),
cross-checked with e2fsprogs 1.47.0. Targets of 1–59 bytes
use the 60-byte inode `i_block` field with a trailing NUL, zero `i_blocks` and
no extent flag. Longer targets use one initialized extent-backed block, also
NUL-terminated. The stored size excludes NUL; block accounting uses 512-byte
sectors. The reader also accepts the canonical legacy one-direct-block symlink.
No general indirect-block file implementation is implied.

Creation sets mode `0120777` and full 32-bit owner IDs before inserting a type-7
directory entry. Reads return target bytes, including bounded offset reads;
regular writes and truncation cannot overwrite the target. Hard links and
rename preserve the symlink inode itself. Unlink and replacement free inline
links without interpreting pathname bytes as block pointers, and free a block
link only after its last name and retained reference are released.

Targets must be nonempty, contain no embedded NUL, and fit in one filesystem
block including their terminator. The public pathname resolver independently
limits expanded pathnames to 255 bytes and 40 link traversals. Encrypted,
EA-backed and inline-data symlinks, malformed block accounting, holes, unwritten
extents and pointers into filesystem metadata are rejected. This does not add
support for arbitrary modern ext4 feature combinations.

## Persistence and failure handling

Block and inode allocation update bitmaps and free counts. Unlink removes the
name immediately; an open inode remains available until its last description
closes, then its blocks and inode are returned to the allocator. Directory
creation/removal updates link counts and group directory counters. Hard links
share the same inode and retain it until the last name and open reference are
released. Rename handles files, directories, replacement, same-inode no-ops and
cross-parent `..` updates. VFS rejects cross-mount moves and mount-point changes;
directory moves reject cycles and non-empty replacement targets.

Namespace mutation is serialized by the VFS lock. If a multi-write rename/link
fails after changing metadata, the mount is quarantined and cannot be marked
clean by a later sync. This preserves failure detection, not rollback or power-loss
atomicity.

Symlink creation initializes its target and inode before directory publication.
Ordinary pre-publication allocation/capacity failures reclaim its own block and
inode; a successful rollback permits later sync. Any I/O failure, including
during rollback, instead quarantines the mount and stops further writes.

Before the first mutation, the filesystem clears the superblock clean bit and
flushes it. Synchronization flushes data/metadata, then marks the volume clean
and flushes again when no unlinked open inode remains. An interrupted mutation
or unclosed orphan leaves an unclean volume; the next writable mount refuses it
and requires offline `e2fsck`. This is not journalled or crash-atomic storage.
Device and detected metadata failures quarantine the mount, reject subsequent
writes/sync, and return EIO rather than a user-pointer fault. If committing the
clean marker fails, the filesystem attempts to persist the dirty marker again;
a permanently failed device cannot guarantee that recovery write. No probe or
mount formats a disk, and errors do not trigger automatic repair.

## Validation

`python3 scripts/test_ext4_fs.py` builds a hosted file-backed block adapter,
mutates/reopens images at all three block sizes and checks each with `e2fsck -fn`.
It covers overwrite, zero write, holes, truncate, open-unlink lifetime,
mkdir/rmdir, rename, hard links, directory topology and repeated allocation/free.
Symlink coverage crosses the 59/60-byte representation boundary, checks the
255-byte and filesystem-block limits, uses `debugfs` upstream fixtures, validates
generated inode flags/accounting/bytes, and includes legacy direct-block unlink,
replacement and retained reference release. A separate hosted cache/durable-disk
model injects read, partial-write and flush failures across the three block
sizes, including symlink creation, link, unlink, rename, replacement and capacity
rollback. Logical extent-capacity
failures also verify quarantine after a partial namespace change. It also checks malformed metadata, pinned/unlinked node lifetime and
32-bit owner IDs. These are failure models, not physical power-loss tests.

The separate OS `scripts/test_persistent_ext4.py` boots a real Virtio disk,
loads `/init` and applications from ext4, writes a note, renames files/directories and creates hard links through native
syscalls, flushes, restarts QEMU on the same image, reads each surviving name,
and cross-checks contents, inode identities and link counts with `debugfs` and `e2fsck`. Its `scope=hardware` means QEMU guest
execution; physical machines and hardware SMP have not been validated.

Run `python3 scripts/test_vfs_namespace.py` for RAM namespace, reference lifetime,
mount isolation and capacity coverage; `python3 scripts/test_process_resources.py`
checks POSIX permissions and retained cwd/dirfd behavior on all ten host profiles.
