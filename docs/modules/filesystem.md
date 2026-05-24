# File System

`ok::fs::VirtualFileSystem` provides a RAM-backed tree of `RamNode` objects.
The initial root contains `/dev` and `/tmp`.

`FileSystemMode` tracks the active filesystem stack:

- `ram_only`: built-in VFS nodes only.
- `ext4_read_only`: EXT4 parser/reader mounted from a block device.
- `ext4_journaled`: reserved mode for journal replay and write support.

The current implementation supports node lookup, creation, read/write, truncate,
unlink, metadata queries, owner/group updates, permission checks, and POSIX
file-descriptor tests through `ok::posix::PosixService`. RAM VFS nodes keep
Linux-style metadata in one
structure: type bits are encoded in `mode`, permission bits remain in the low
12 bits, and `uid`, `gid`, link count, block size, and allocated block count are
reported through `stat`. File payload storage is separated from directory/node
metadata so directory-heavy trees do not reserve a 4 KiB data buffer per node.
`open`, `cat`, directory listing, creation, unlink, and directory removal route
through effective uid/gid access checks; parent directories require write and
execute permission for creates and removals. `/tmp` is initialized as `01777` so
normal users can create temporary files. Future work should add mounts, full
path normalization, and mount routing.

`ok::fs::SimpleDiskFileSystem` is the first block-backed writable filesystem.
It formats and mounts any `ok::driver::BlockDevice` with 512-byte sectors. The
on-disk layout is deliberately small: block 0 is a superblock, blocks 1-4 are a
fixed directory table, and file data is allocated as contiguous extents after
that table. It currently supports a flat root directory with create, unlink,
list, stat, whole-file read, and whole-file write. This gives the kernel a real
disk-management path before VFS mount routing and before full EXT4 write support.
SimpleFS directory entries persist the same Linux-style mode/owner/link metadata
fields, so the POSIX facade and debug shell see consistent type and permission
data across RAM VFS and block-backed files.

`ok::fs::Ext4Volume` is a read-only EXT4 foundation. It validates the
superblock magic, parses block size, inode size, extent support, basic counters,
and volume name, and can read raw filesystem blocks from either an image span or
any `ok::driver::BlockDevice`. The block-device mount path is the interface real
ATA/NVMe/virtio-blk drivers use. Directory walking, inode data reads,
journal replay, allocation, and writeback remain follow-up layers.
