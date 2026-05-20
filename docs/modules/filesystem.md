# File System

`ok::fs::VirtualFileSystem` provides a RAM-backed tree of `RamNode` objects.
The initial root contains `/dev` and `/tmp`.

`FileSystemMode` tracks the active filesystem stack:

- `ram_only`: built-in VFS nodes only.
- `ext4_read_only`: EXT4 parser/reader mounted from a block device.
- `ext4_journaled`: reserved mode for journal replay and write support.

The current implementation supports node lookup, creation, read/write, truncate,
unlink, metadata queries, and POSIX file-descriptor tests through
`ok::posix::PosixService`. Future work should add mounts, permissions, path
normalization, and a block-backed writable filesystem.

`ok::fs::Ext4Volume` is a read-only EXT4 foundation. It validates the
superblock magic, parses block size, inode size, extent support, basic counters,
and volume name, and can read raw filesystem blocks from an image span. Directory
walking and inode data reads are intentionally separate follow-up layers.
