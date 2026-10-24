# Native file identity

UAPI 0.7.0 adds `OK_SYS_FSTAT_V2` (1026) and `OK_SYS_NEWFSTATAT_V2`
(1027). They return `struct ok_stat_v2`: the unchanged `struct ok_stat`
prefix followed by a 64-bit inode identity. The descriptor call takes `(fd,
output)`; the pathname call takes `(dirfd, path, output, flags)` and shares
the existing `newfstatat` resolution and flag semantics.

The provider is queried once. The inode is copied only when its metadata
advertises `fs::Attribute::inode`; otherwise the wire value is zero. Padding
is zero, including padding between the legacy prefix and the inode. The
native user-copy adapter validates the complete target-specific v2 output
range before dispatch and publishes it only after a successful query.

Legacy `stat`, `lstat`, `fstat` and `newfstatat` keep their original numbers
and write exactly `sizeof(ok_stat)`. Statically linked legacy native programs
retain that interface. The Linux syscall personality and its architecture
specific stat layouts are unchanged. SDK45 consumers compile against the
exported target headers; a UAPI version label cannot substitute for artifact
hash, architecture, mode and compiler admission.

Inode alone is not a globally unique file identifier across mounts. Device
identity, timestamps and additional native stat metadata are not exposed by
this record. The distribution's new static libc uses the v2 calls and a
64-bit `st_ino` on all targets. That libc/header pair must be rebuilt together,
especially on 32-bit targets; this is not a shared libc ABI promise.

Hosted projection tests cover identity validity, full-width values, padding,
legacy bounds and retained descriptors. Native-copy tests exercise the real
adapter with hosted address mappings; they are not user-mode execution proof.
Standalone QEMU and unprivileged ELF qualification remain separate gates.
