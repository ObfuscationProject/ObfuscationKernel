# File System

`ok::fs::VirtualFileSystem` provides a RAM-backed tree of `RamNode` objects.
The initial root contains `/dev` and `/tmp`.

The current implementation is enough for syscall and module tests. Future work
should add file descriptors, mounts, permissions, path normalization, and a
block-backed filesystem.

