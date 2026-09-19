# Device-backed ext4

`Ext4FileSystem` is a kernel filesystem mechanism over the runtime `BlockDevice`
service boundary. `Ext4Node` integrates lookup, listing, permissions, creation,
read/write, truncate, unlink/rmdir and synchronization with the generic VFS mount
namespace. The OS owns Virtio transport drivers, image creation and root selection.
The old `Ext4Volume` metadata reader and RAM-backed block models remain separate.

## Supported on-disk format

The initial writer deliberately supports a bounded, non-journalled ext4 subset:

- 512-byte device sectors; 1, 2 or 4 KiB filesystem blocks; 128/256-byte inodes.
- At most 32 block groups, 32-bit filesystem block addresses and file sizes.
- `extent`, `filetype`, `sparse_super`, `large_file` features only.
- Extent tree reads up to depth three; writes/truncate require inline extents,
  at most four physical runs. Sparse holes read as zeros. Initialized extents
  beyond EOF and unwritten-extent mutation are explicitly unsupported.
- Regular files and ordinary linear directories; Linux-layout 32-bit uid/gid updates; other creator OS layouts are rejected.
- 128 cached nodes, 31-byte component names, 64-entry directory listing buffers.

Journal/JBD2 replay, metadata checksums, 64-bit group descriptors, indexed
directories, bigalloc, inline data, symlink traversal, rename and hard-link
creation are not implemented. Unsupported feature bits are rejected before any
write. The default modern `mke2fs -t ext4` feature set is therefore not writable
by this implementation. ObfuscationOS `tools/mkext4.py` produces the supported
format explicitly, with initialized inode tables and no journal.

## Persistence and failure handling

Block and inode allocation update bitmaps and free counts. Unlink removes the
name immediately; an open inode remains available until its last description
closes, then its blocks and inode are returned to the allocator. Directory
creation/removal updates link counts and group directory counters.

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
mkdir/rmdir and repeated allocation/free. A separate hosted cache/durable-disk
model injects 636 read, partial-write and flush failures across the three block
sizes. It also checks malformed metadata, pinned/unlinked node lifetime and
32-bit owner IDs. These are failure models, not physical power-loss tests.

The separate OS `scripts/test_persistent_ext4.py` boots a real Virtio disk,
loads `/init` and applications from ext4, writes a note through native syscalls,
flushes, restarts QEMU on the same image, reads the note, and cross-checks the
contents with `debugfs` and `e2fsck`. Its `scope=hardware` means QEMU guest
execution; physical machines and hardware SMP have not been validated.
