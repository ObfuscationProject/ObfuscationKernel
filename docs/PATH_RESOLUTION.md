# Bounded object pathname resolution

This is the first P08 implementation slice, retaining the bootstrap fixed pools.
It does not establish SMP safety or complete POSIX filesystem semantics.

A `PathHandle` identifies a node and its mount instance. A `ResolveContext`
supplies a root boundary, a starting directory and credentials. Null root/start
select the namespace root. Absolute paths and absolute symlink targets restart
at the supplied root; relative targets restart at the link's containing directory.
The state machine checks directory type and search permission before consuming
each component, including `.` and `..`. Parent traversal crosses mount roots
through their stored covered-directory handle and clamps at the supplied root.
Mount traversal matches object identity and mount instance, never a reconstructed
cwd string. Mount installation rejects symlinks in its canonical path, avoiding
an alias becoming the stored mount location. Only `getcwd` reconstructs a display path. Root and start handles are trusted
kernel inputs; callers supplying a restricted root must provide a start beneath
it. This API does not expose a userspace chroot operation.

Final-component policies are follow, no-follow, parent-for-mutation and
follow-to-create (including dangling symlinks). Parent
resolution returns an exact directory and leaf while preserving trailing slash
requirements. Namespace mutations resolve, check access and update under the
existing VFS lock. Resolved node references are retained before releasing that
lock and released by a move-only result. File creation for open returns the
newly retained object without a second pathname lookup. Cwd and directory descriptors keep their
existing retained node ownership. Permission changes through direct node APIs
and filesystem-internal readers are not covered by a global concurrency claim.

Symlink expansion uses at most 40 links and a 255-byte pending pathname. RAM
symlink creation is atomic with publication. Device-backed ext4 uses Linux-format
inline or single-block symlinks, initializes ownership and target before
publication, and quarantines partial I/O failures; it is not crash-atomic.
See [ext4 storage](modules/ext4-storage.md) for its bounded format and hosted
interop/failure tests. Search permission
starts at the held cwd/dirfd, so revoked ancestor access does not invalidate an
existing directory reference. A detached directory cannot gain new entries and
cannot resolve through a replacement name; its `.` reference remains usable.

Hosted regressions cover renamed and covered starts, duplicate mount roots,
root boundaries, search permissions, final policy, trailing slash and symlink
cycles. These are deterministic namespace/state interleavings, distinct from
SMP races and native syscall execution.

Validation: `scripts/test_vfs_namespace.py`, `scripts/test_process_resources.py --arch all`, `scripts/test_archive.py` and `scripts/test_kernel_services.py`.
The kernel roadmap symlink test now verifies real traversal and no-follow reads;
its classification remains a kernel namespace test. Native OS acceptance is a
separate SDK consumer gate.

Remaining P08 work includes general dentry/mount
lifetimes and unmount, supplementary groups and full credential rules, concurrent
chmod versus namespace operations, per-filesystem lock ordering, larger paths and
pools, timestamps, inode/device IDs, and remaining filesystem/descriptor options.
