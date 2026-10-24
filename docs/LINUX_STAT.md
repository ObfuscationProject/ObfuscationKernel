# Qualified Linux stat subset

Status: SDK21 candidate implementation under qualification. Native UAPI remains
0.6.0. Hosted models and target compilation are separate from actual user-mode
execution; this document does not declare SDK21 delivered or the workstation
plan complete.

## Supported projection

The Linux personality implements 256-byte `statx` on all ten profiles. It also
implements the 144-byte kernel `stat`, `lstat`, `fstat` and `newfstatat` records
on x86_64, and the 128-byte `fstat`/`newfstatat` records on AArch64 and RV64.
The other seven musl 1.2.5 ports use statx. Their raw legacy records retain
ENOSYS, including MIPS64's time32 kernel-stat layout. Public libc `struct stat`,
raw Linux records and native `ok_stat` remain distinct contracts.

Syscall and errno numbers come from the source-derived ABI inventory. A known
provider refusal returns EOPNOTSUPP (95, or 122 on MIPS/MIPS64), preserving the
global native unsupported-to-ENOSYS policy. MIPS O32 captures statx's fifth
argument through the guarded stack copier before inspecting flags or pointers.

## Provider admission

Each successful result requires one checked common FileStatus snapshot with
qualified nonzero inode/device identity, matching regular/directory/symlink mode,
normalized atime/mtime/ctime with consistent readonly-ext4 or maintained-ext4
provenance, and the `Attribute::blocks512` validity bit. Device 0:0 is unavailable;
0:nonzero and nonzero:0 are representable. A held unlinked inode may have zero
links. Signed libc size/block counts and block-size bounds are checked even for
statx's unsigned wire fields. Every output byte and padding field is initialized.

Ext4 allocation qualification is independent of mount writability and timestamp
validity. The first admitted subset has canonical inline depth-zero extents or
supported fast/one-block symlinks, no unsupported inode flags/EA/high fields,
ordered logical ranges, bounded non-metadata physical ranges without overlap,
and an exact allocated-block sum equal to i_blocks_lo. Holes consume no blocks.
Valid but unsupported reduced extent capacities or deeper trees leave allocation
qualification unavailable; detectable malformed counts, ranges or headers return
EIO. The existing writer admission is unchanged.

Readonly ext4 and explicitly maintained nonjournalled ext4 may qualify. Default
writable/RAM/archive/FAT/pipe/console/device metadata remains unavailable for
Linux stat. Metadata queries do not sample a clock or update atime. Following
a symlink can perform the provider's normal maintained-atime event.

## Argument and copy order

1. Capture all required register/stack arguments once.
2. Reject unsupported flags and reserved statx mask bit 31 before path access.
3. Copy one bounded NUL-terminated pathname, or use NULL as empty only with
   AT_EMPTY_PATH. Empty without that flag returns ENOENT.
4. Resolve through common cwd/dirfd identity and obtain one final checked
   snapshot. Direct fd and empty ordinary-fd forms verify a live descriptor and
   open description before fstat. Empty AT_FDCWD uses the held cwd object.
5. Check provider qualification and representability, then encode in kernel
   storage. Provider and lookup errors precede output-pointer EFAULT.
6. Admit the whole fixed output range and perform one guarded copyout.

Newfstatat accepts NOFOLLOW, NO_AUTOMOUNT and EMPTY_PATH. Statx additionally
accepts FORCE_SYNC or DONT_SYNC, never both. Those sync flags query the same
local snapshot; they do not imply a filesystem flush. Unknown future mask bits
are ignored without returning their capabilities. Complete BASIC_STATS (0x7ff)
is required even for an empty request mask because musl copies all basic fields.
Birthtime, attributes, mount IDs and newer optional fields remain zero with bits
clear. Unknown-flag handling is deliberately uniform and differs from Linux's
empty-fd fast-path quirks.

The existing limits remain 255 pathname bytes plus NUL and 31 bytes per path
component. A successfully followed symlink's atime event remains committed if
later output admission fails. No concurrent unmap/SMP or physical copy-fault
atomicity claim follows from the serialized boot-CPU copy boundary.

## Reproduction and outstanding execution

`scripts/test_linux_stat.py --output <new-directory>` runs codec, argument,
saved-frame, actual dispatcher and allocation models through xmake. Add
`--sanitize` for ASan/UBSan. All are hosted tests with modeled user memory.
`scripts/test_linux_bootstrap.py` instantiates the actual adapter for all ten
target compilers. Kernel debug tests exercise the selected codec as a model.

`scripts/test_linux_stat_blocks_oracle.py --output <new-directory>` creates
scratch images offline with e2fsprogs and compares raw allocation metadata.
Pass that output using `--oracle` to `test_linux_stat_provider.py`,
`test_linux_stat_service.py` and `test_linux_stat_copy_order.py`, each with its
own new `--output`. The image runners accept `--sanitize`; none mounts a host
filesystem. Target-layout goldens retain independently extracted layout data in
`docs/abi/linux-stat-target-layouts.json`.

Before delivery, the exact SDK21 Kernel producer and OS artifact consumer must
pass all-ten Debug/Release native/musl fixtures, raw syscall boundary controls,
negative and post-2038 epochs, readonly/default-provider controls and Linux
reference execution. OS owns those fixtures, user binaries and distribution
policy. Default-root timestamp adoption, utimens, journalling, full POSIX/ext4,
physical machines, SMP and the remaining M0-M12 work stay separate.
