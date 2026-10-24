# Linux process creation mask

The Linux personality appends umask to the generated all-ten operation table and
forwards one narrowed unsigned32 argument to the existing native/common service.
The per-process resource owns the mask, initialized to0022. Only0777 is stored;
the old mask is returned, including zero. No pointer or user-memory copy is part
of this operation. This operation originated in SDK16/UAPI0.4.0; the current
directory-creation integration requires matching SDK/module18 and native UAPI0.5.0 artifacts.

Common create_file/mkdirat apply the mask at creation. Reopening/truncating an
existing inode preserves its mode. Ordinary fork copies the value while retaining
shared open descriptions independently; later child changes do not alter the
parent mask. Actual exec preserves the resource's mask. The separate SDK18
directory-creation integration now admits Linux mkdir/mkdirat and common parent
setgid inheritance; see `LINUX_MKDIR.md` for its own qualification status.
Default ACLs, CLONE_FS/thread sharing and SMP atomicity remain separate contracts. Mask changes do not qualify filesystem timestamp maintenance.

The generated provenance pins Linux6.13.4 kernel/sys.c and musl1.2.5 umask.c.
Hosted actual-adapter/native tests exercise all ten register conventions with
zero,0777, all-ones and high-register patterns, unchanged existing modes, common
file/directory creation, native sharing and fork isolation under ASan/UBSan.
The standalone Debug test checks common creation modes and cleanup separately.

The cumulative musl probe runs after directory-stream checks. Each of its two
executions creates a distinct O_EXCL slot containing parent, restricted, fork
child and real exec child files. The OS runner requires both complete slots and
uses offline debugfs to verify eight distinct regular inodes, exact modes and
contents, plus the existing fsck and cumulative checks. Parent O_TRUNC is a
separate assertion that an existing inode is not remasked. The parent mask is
restored before exit. An exact generation15 kernel must fail at the first public
umask after passing directory and all earlier gates.

The corrected x86_64 Debug prototype passed14 producer/consumer commands and two
cumulative musl executions, with eight distinct persisted regular-file identities,
exact modes and data. An independent review audited the archived source, SDK,
logs and disk, and confirmed the exact ABI15 ENOSYS negative. The earlier prototype
used a wrong exec pathname; its failed artifacts remain separate.

The final ABI16 and public-clock ABI17 unions each passed their own all-ten
Debug/Release SDK/native qualification. Earlier isolated generation16 prototype
evidence remains bound to its archived inputs. Every future combined generation
requires fresh qualification. Hosted checks are not guest execution; QEMU does
not establish physical-platform or SMP support.
