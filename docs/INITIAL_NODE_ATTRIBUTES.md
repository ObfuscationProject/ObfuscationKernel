# Initial node attributes and namespace publication

This shared SDK/module18 boundary keeps native UAPI0.5.0. All-ten hosted models,
target compiles and the frozen Debug/Release native/musl matrix passed, followed
by a fresh shared x86 SDK consumer. Final disk bytes were independently checked;
these bounded results do not complete M2 or establish crash atomicity.

`Node::create_child(name, InitialNodeAttributes)` accepts a regular-file or
directory type, final permission/special mode, and full unsigned32 UID/GID.
Success returns a stable provider-owned borrowed node. Unsupported providers
reject before mutation; callers never emulate the operation with legacy create,
lookup, chmod and chown. VFS checks a null success result as an I/O error.
The caller still owns any subsequent retain operation and its possible failure.

The common namespace lock serializes parent resolution, admission and runtime
publication. Search checks precede duplicate lookup, which precedes parent write
admission. The process service applies its0777 mask once. VFS inherits a setgid
parent's GID for regular files, directories and symlinks, and propagates the
setgid bit only to directories. Native requested special bits remain deliberate;
the Linux adapter strips caller setuid/setgid/type bits before common creation.
RAM nodes receive final attributes before attachment. No separate Linux fsuid,
fsgid, capabilities, supplementary groups or default ACL contract is implied.

The existing nonjournalled ext4 provider reserves a stable node-cache slot and
plans parent insertion before disk allocation. It owns every inode, child block
and optional parent-growth block until publication. Logical capacity failure
rolls back checked allocations before permitting further writes. Node cache
exhaustion is ENOMEM; inode/block/inline-extent exhaustion is ENOSPC; directory
link saturation is EMLINK. Synthetic exhausted-bitmap fixtures are not claimed
to be fsck-clean ordinary images.

The new inode contains final mode, low/high UID/GID, extents, size and links
before its parent directory entry is written. The successful returned node
requires no fallible lookup. After any uncertain transport write, failed flush,
or later publication failure, the existing failed state prevents further writes
and sync. It does not generally prohibit all reads. A failure may occur after
the child has become visible with correct attributes; this is not crash-atomic
namespace journalling or an all-errors rollback promise.

The separately qualified mounted-metadata provider rejects ordinary creation.
The new operation uses its existing admission gate: unsupported while readable,
busy during metadata publication, and denied after quarantine, without device
calls. Its persistent transaction owner is not repurposed for directory writes.
The status vocabulary does not yet distinguish Linux EROFS from unsupported.

`scripts/test_initial_child.py` covers common RAM/provider boundaries;
`scripts/test_ext4_initial_attributes.py` covers six block/inode layouts,
independent raw inode/directory bytes, logical rollback and transport faults.
`scripts/test_linux_mkdir.py` exercises all ten adapters with hosted user mappings.
The OS `--require-mkdir` gate executes two cumulative musl creation runs and
independently rereads19 ext4 inodes and four data payloads. These evidence layers
remain distinct. LSan cannot run under the available ptrace environment; ASan
and UBSan use `ASAN_OPTIONS=detect_leaks=0`. SMP and physical execution remain open.
