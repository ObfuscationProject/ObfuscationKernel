# Linux personality limits for the musl/BusyBox gate

Slice of `feat/linux-personality` (Kernel issue #6, paired ObfuscationOS
`feat/linux-personality`). The WT1.5 BusyBox gate ran into kernel limits that
ObfuscationOS worked around with patches, held-back applets and a size-checked
recipe ([MUSL_USERLAND](../../../ObfuscationOS/docs/qualification/MUSL_USERLAND.md)).

## Changes

| Limit | Now |
| --- | --- |
| `execve` copied at most 16 argv and 16 envp strings under 256 bytes | Strings live in one kernel object run (`Arguments`): each with its NUL at most 128 KiB (`MAX_ARG_STRLEN`), all at most 256 KiB, 2048 per vector; `E2BIG` beyond. The initial stack grows with them; the pointer vectors and auxiliary vector form one 16-byte-aligned record |
| A non-ELF file returned `EINVAL`; no `#!` | A file without the ELF magic returns `ENOEXEC`. A file whose first line is `#!interpreter [argument]` runs as `interpreter [argument] path argv[1..]`, through at most four interpreters (`ELOOP` beyond), as Linux `binfmt_script` does; the script needs an execute bit and execute access |
| No `/dev/null`, `/dev/zero` | `fs::DeviceFileSystem` (`include/ok/fs/devfs.hpp`): `null`, `zero` and `full` character devices, constant-initialized, with `kernel_fixed` epoch timestamps that Linux `stat` admits; ObfuscationOS mounts it on `/dev` |
| `CLONE_VM\|CLONE_VFORK` returned `ENOSYS` | `vfork` and `clone(CLONE_VFORK [\| CLONE_VM])` with the child exit signal run as a copy-on-write fork, on the caller's new stack when one is given. The child has its own copy of memory, so a program cannot tell except that the parent is not suspended |
| `uname`, `sysinfo`, `link`, `symlink`, `fchownat` not routed | The generated Linux ABI tables gain `uname`, `sysinfo`, `link`, `linkat`, `symlink`, `symlinkat`, `chown`, `lchown`, `fchown`, `fchownat` and their 32-bit-ID variants, `vfork`, `statfs`, `statfs64`, `fstatfs`, `fstatfs64`, `utimensat`, `utimensat_time64`, `mknod` and `mknodat` on all ten profiles. The adapter encodes Linux `utsname` (`Linux`, release `6.13.0-obfuscation`, the profile's machine) and `sysinfo` (discovered frames), forwards links and symlinks to the native calls and maps `chown` IDs (`-1` keeps the current one; profiles with `chown32` keep 16-bit legacy calls) onto `change_owner`. The non-journalled ext4 writer gains `change_owner` (root only; set-ID bits cleared as Linux does) |
| `utimensat` not routed | `fs::TimestampChange` and `Node::set_times`; the non-journalled ext4 writer sets them on maintained mounts. `PosixService::utimensat` follows Linux permission rules; the adapter decodes time32 and time64 records, `UTIME_NOW`, `UTIME_OMIT` and a NULL path |
| `statfs` not routed | `Node::filesystem_statistics`; the non-journalled ext4 writer reports its superblock counters. The adapter encodes `statfs` (LP64) and `statfs64` (ILP32) in each architecture's layout, including the mips orderings and arm's packed record |
| A directory listing held 64 entries; `getdents` of a larger directory failed with `EOVERFLOW` | `Node::list_from` returns the listing that starts at a stream position; both ext4 providers resume their scan there and the POSIX directory stream fetches pages as it advances |
| ppc admission read at most 512 KiB, MIPS ABI flags at most 128 KiB | When an admission reports a prefix shortage the loader reads the requested prefix, up to 16 MiB, into a kernel object run |

`StatusCode` gains `exec_format` (`ENOEXEC`) and `argument_list_too_long`
(`E2BIG`); `TimestampSource` gains `kernel_fixed`; `Node` gains `list_from`,
`set_times` and `filesystem_statistics`. These change exported headers and the
`Node` vtable, so under PLAN rule 2 the merge needs a module ABI bump that the
paired ObfuscationOS branch consumes.

## Still open

- **Path component length.** `max_path_segment` stays 32 (31-byte names).
  `DirectoryEntry` names, the journalled writer's 16 retained names per node
  and SimpleFS's on-disk name capacity are sized by it, and listings are
  returned on the kernel stack; 255-byte names need those decoupled first.
- **FIFOs.** `mknod`/`mknodat` remain unrouted: named pipes need FIFO nodes in
  the VFS and the providers. `mkfifo` and `mknod` stay held back.
- **`/proc`.** No process filesystem: `ps`, `top`, `free` and `kill`-by-name
  stay held back, and `df` needs a mount table (`/proc/mounts` or
  `/etc/mtab`) as well as `statfs`.
- **Journalled root.** The journalled ext4 writer still refuses hard links,
  symbolic links, `set_times` and `filesystem_statistics`; the guest's default
  root uses the non-journalled writer.

## Tests

Hosted (`scope=model`): `scripts/test_native_exec_arguments.py` (arena growth,
prepend/replace, `MAX_ARG_STRLEN`, `ARG_MAX` and per-vector bounds, nested
`#!` interpreters and the four-level limit, execute-bit refusal, `ENOEXEC`),
plus the existing runtime suites on the integration tree.

QEMU guest (`scope=hardware`): `tests/userland/sh-acceptance.sh` gains
`exec_args`, `exec_env`, `exec_script`, `exec_enoexec`, `dev_null`,
`dev_zero`, `dev_full`, `uname`, `uptime`, `hostname`, `links`, `chown`,
`touch`, `statfs`, `listing`, `listing_glob`, `bc` and `bzip2`. Results per
profile are in the paired ObfuscationOS note.
