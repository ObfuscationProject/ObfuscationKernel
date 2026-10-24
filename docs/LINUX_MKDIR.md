# Linux directory creation

The bounded Linux bootstrap adapter admits `mkdir` and `mkdirat` through the
common `PosixService::mkdirat` and VFS creation path. The generated operation enum
appends both operations after `umask`; existing operation values are unchanged.

| Profile | mkdir | mkdirat |
| --- | ---: | ---: |
| i386 | 39 | 296 |
| x86_64 | 83 | 258 |
| aarch64 | unavailable | 34 |
| arm32 | 39 | 323 |
| rv64 | unavailable | 34 |
| rv32 | unavailable | 34 |
| loongarch64 | unavailable | 34 |
| mips O32 | 4039 | 4289 |
| mips64 N64 | 5081 | 5248 |
| ppc32 | 39 | 287 |

`scripts/generate_linux_abi.py` checks each available number against both the
SHA-256-pinned musl 1.2.5 archive and inspected Linux 6.13.4 source. The manifest
`docs/abi/linux-profiles.json` records individual file hashes, including musl's
`src/stat/mkdir.c`, `src/stat/mkdirat.c`, `include/sys/stat.h`, and Linux's
`fs/namei.c`, `fs/inode.c`, `include/linux/types.h`. Linux source file identity
does not establish the authenticity of a full upstream Linux archive.

The pathname is copied through checked user access before the common service
examines the descriptor. Copies stop at the first NUL and admit at most 255
non-NUL bytes. The common namespace currently limits each component to 31 bytes.
`mkdirat` takes a signed low-32-bit descriptor, and the pathname pointer has the
selected target's word width. Both calls fit in registers on every profile;
MIPS O32 does not read a stack argument. Absolute paths ignore the descriptor;
an empty copied pathname returns ENOENT before a bad descriptor is examined.

Linux's `umode_t` is unsigned 16-bit. The adapter further retains only `01777`
(rwx and sticky), leaving one application of the process umask to the common
service. Caller setuid, setgid, and type bits do not pass through. The common
VFS adds a setgid parent's group ID and, for directories, its setgid bit before
publication. A regular child inherits the group without automatically gaining
the setgid bit. Ownership follows the common effective UID/GID model, which
does not yet model separate Linux fsuid/fsgid, supplementary groups, ACLs, or
capabilities. Native directory mode policy remains independent.

The final existing entry is not followed. Existing directories, files, or
dangling symlinks return EEXIST, including a trailing slash. Search permissions
still apply during traversal; for a searchable parent, final duplicate lookup
precedes write permission admission. Creation sends complete initial metadata
to `Node::create_child`; success publishes complete initial attributes, and
there is no legacy create/chmod/chown fallback. Nonjournalled ext4 I/O failures
may occur after publication and terminate further writes; they do not promise
crash atomicity or complete rollback. Providers without the new
operation return ENOSYS through the current unsupported status; this slice does
not claim Linux EROFS behavior for read-only providers.

Run `python3 scripts/test_linux_mkdir.py` and
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/test_linux_mkdir.py --sanitize`.
The tests cover all ten profiles and all sixteen declared syscall forms using
the real adapter/common service with hosted user-copy mappings. They check
target return conventions, scalar truncation, mode masking, umask, credentials,
setgid inheritance, descriptor semantics, duplicate/permission ordering,
symlink and trailing-slash behavior, mapping/page boundaries, inaccessible
path bytes, pathname limits, and provider error propagation without fallback.
`scripts/test_linux_bootstrap.py` separately compiles the adapter for all ten
target architectures when its explicit toolchain paths are available. Hosted
models and target object compilation do not establish privileged execution,
guest musl execution, concurrency safety, SMP, or physical hardware support.


## Qualified bounded integration

The shared SDK/module18 and native UAPI0.5.0 source passed all-ten Debug/Release
producer/consumer qualification (470 commands,40 cumulative musl executions),
16 additional date boots/32 executions and a fresh shared x86 rebuild. Every
mkdir boot checks19 distinct ext4 nodes/four file payloads after execution;
the native x86 output also passes an actual Linux readonly mount with unchanged
image bytes. The ABI17 negative stops at the first mkdir with ENOSYS. Hosted
models, target compilation, actual QEMU execution and Linux interoperability
remain separate evidence. The initial signed-debugfs-GID parser failure is
retained, and initial mutable guest disk bytes are unavailable for independent
replay. See the OS implementation progress for receipt locations and limits.
