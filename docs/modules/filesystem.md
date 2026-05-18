# File System

`ok::fs::VirtualFileSystem` provides a RAM-backed tree of `RamNode` objects.
The initial root contains `/dev` and `/tmp`.

The current implementation is enough for syscall and module tests. Future work
should add file descriptors, mounts, permissions, path normalization, and a
block-backed filesystem.

`ok::fs::Ext4Volume` is a read-only EXT4 foundation. It validates the
superblock magic, parses block size, inode size, extent support, basic counters,
and volume name, and can read raw filesystem blocks from an image span. Directory
walking and inode data reads are intentionally separate follow-up layers.
