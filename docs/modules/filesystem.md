# File System

`ok::fs::VirtualFileSystem` provides a bounded mount namespace over the runtime
`Node` interface. Its initial RAM tree contains `/dev` and `/tmp`; an OS boot
extension can mount a device-backed root before native `/init` starts. The
kernel owns filesystem mechanisms, while ObfuscationOS owns external block
drivers, disk-image creation and root-selection policy.

## VFS and file semantics

The mount table holds eight roots. Lookup normalizes `.` and `..` before
selecting the longest matching mount path, so `/mount/../tmp` resolves through
the parent namespace. Lookup, creation, directory listing, removal, metadata,
read/write, truncate and sync delegate to the selected filesystem's nodes.
Mounted nodes are not cast to `RamNode`. Unmount and user-facing mount syscalls
are not implemented.

`FileSystemMode` records the configured stack rather than performing a mount:

- `ram_only`: the initial built-in VFS tree.
- `ext4_read_only`: the existing read-only ext4 profile label.
- `ext4_read_write`: the OS-selected non-journalled ext4 root.
- `ext4_journaled`: reserved; journal replay is not implemented.

Metadata carries Unix type/permission bits, uid/gid, link count, block size and
allocated block count. The POSIX service checks effective uid/gid permissions;
create and removal require write/execute access on the parent directory. `/tmp`
is initialized as `01777`. RAM file buffers are separate from node metadata:
the RAM tree has 128 nodes, 32 writable payload buffers of 4096 bytes, and at
most 64 children per directory. Component names are limited to 31 bytes.

Overwrites preserve bytes beyond the written range, zero-length writes preserve
file size, reads beyond EOF return zero, and extension fills holes with zeros.
`O_TRUNC` and whole-file replacement explicitly truncate. Native `readv`/`writev`
use checked user buffers; `pread`/`pwrite` preserve the shared open-file offset.
`fsync` and `fdatasync` reach the node's synchronization operation. RAM nodes
have no persistent backing; ext4 issues device FLUSH.

## Device-backed ext4

`ok::fs::Ext4FileSystem` mounts clean supported ext4 volumes through
`ok::driver::BlockDevice`. It supports extent-backed regular files and linear
directories, create/read/write/truncate/unlink, mkdir/rmdir, chmod/chown,
directory enumeration and synchronization. Unlinked open inodes remain alive
until the last open-file description closes.

The initial writer accepts 1/2/4 KiB blocks, 128/256-byte inodes and at most
32 block groups. Extent reads support trees up to depth three; mutations require
inline extents with at most four physical runs. Journals, metadata checksums,
64-bit group descriptors, indexed directories, symlink traversal, rename and
hard-link creation are unsupported. Mount rejects unsupported features before
writes. See [Device-backed ext4](ext4-storage.md) for the complete format and
capacity limits.

Before mutation, ext4 marks the superblock unclean and flushes it. Successful
synchronization marks it clean only when no unlinked open inode remains.
Interrupted mutation requires offline `e2fsck` before a writable mount; the
implementation does not provide journal recovery or crash-atomic updates.

ObfuscationOS supplies real Virtio disk transports for i386, x86_64, arm32,
aarch64, rv32, rv64 and loongarch64. MIPS/MIPS64/PPC currently use the archive
root and need PCI host bridge transport support. Device and reboot-persistence
acceptance use actual QEMU guest I/O, with separate hosted filesystem tests;
physical hardware and hardware SMP storage have not been validated.

## Earlier storage foundations

`ok::fs::SimpleDiskFileSystem` remains a separate flat filesystem over a
512-byte-sector `BlockDevice`: block zero is its superblock, blocks 1–4 hold a
fixed directory table, and file data uses contiguous extents. It supports
create/unlink/list/stat and whole-file reads/writes with Unix-style metadata.
The standalone kernel exercises it with RAM-backed devices. Those tests do not
demonstrate persistent access to an attached QEMU disk, and SimpleFS does not
provide the distribution's mounted writable root.

`ok::fs::Ext4Volume` remains a separate read-only metadata/block reader. It
validates the superblock and reads raw blocks from an image span or block
device; inode traversal and writable VFS nodes are implemented by
`Ext4FileSystem`, not by this older reader.
