# Bounded mounted ext4 metadata policies

`Ext4MountedMetadata` is an explicit side-mount provider for the checked 1 KiB,
checksum-v3 journal profile accepted by the existing metadata writer. Mount
admission requires clean filesystem and journal controls and an empty log. It
does not change `Ext4FileSystem` feature masks or automatic boot-root policy.

One private BlockSessions session and eight canonical buffers live for the
provider lifetime. The default `Ext4MountedPolicy::maintenance_only` retains
unqualified stored metadata. Its internal trusted `update_metadata` operation changes one
ordinary regular inode's mode, uid, gid, explicitly supplied epoch ctime and
version. It performs one synchronous journal publication and checkpoint. Under
this default policy public chmod/chown, data writes, truncate and namespace changes
remain unsupported.
The supplied timestamp is control input, not a qualified machine epoch clock.

Public reads are admitted through a short provider gate. New reads and updates
are refused while an operation is active. The old checked view is retired before
publication. Renewal compares the complete layout and metadata domains, private
session identity, provider epoch, and every changed home and canonical block
against the exact sealed checkpoint image. Only then is a fresh checked view
published. Node, open-description, forked-descriptor and alias identities remain
stable through repeated successful operations. Local capacity/policy errors do
not poison a healthy view; transport/integrity failures quarantine the provider
and session, including transport statuses that resemble normal namespace errors.

The public owner retains its one-shot interface and three-buffer default. Only
the mounted provider can reconstruct a drained private operation; session serial,
cache generations and transaction serial are not reset. No raw session, arbitrary
image plan or durable-publication token is exposed. A published provider cannot
close, reset or unmount. Provider, registry, device and backing storage must
outlive every published node and remain alive through quarantine and teardown.

## Checked metadata consumers

The qualified VFS/POSIX path uses `Node::query_metadata()` for open, stat/fstat,
pathname traversal, directory listing, descriptor retention and executable
admission. The projection decodes the same checked inode as the reader and
returns actual `i_blocks`, block size and inode identity. Under the default policy timestamp-valid flags and blocks512 qualification
remain unset; read-only timestamp provenance cannot describe a mutable provider.

`Node::metadata()` cannot report an error. Its mounted implementation is a legacy
diagnostic accessor whose failure fallback has mode zero and no valid inode
identity; it is not a successful metadata query or qualified quarantine evidence.
The source audit found remaining direct metadata calls in RAM-owned child lists,
RAM archive replacement, initial archive directory setup, and in-filesystem
`Ext4FileSystem::metadata(inode)` helpers. Initial archive setup precedes explicit
side-mount publication; it is outside this mounted interface's qualified path.
Runtime diagnostics or future callers needing mounted data must use the checked
operation and propagate its status.

## Checked maintained mode changes (SDK30)

Construct `Ext4MountedMetadata<>(Ext4MountedPolicy::maintained_noatime)` to select
an immutable maintained-metadata policy. Mount admission streams checked inode
bitmaps and validates stored atime/mtime/ctime on root inode 2 and every allocated
ordinary inode at or above first_ordinary_inode. Reserved system/journal inodes
remain outside public-node qualification. Unsupported allocated ordinary inode
representations reject the selected mount. The reader allows at most 32 groups
and 8,192 inode candidates per group for this 1 KiB profile (262,144 candidates);
the scan uses fixed scratch space and allocates no per-inode index. Reads do not
update atime. Stat needs valid stored fields but no active clock.

The checked view's new `query_stored_metadata` projects metadata, checked i_blocks
and all three timestamps from one validated inode image. Its timestamps remain
unqualified until the maintained owner labels them maintained_ext4. Generic
`query_metadata` retains the previous unqualified behavior. VFS supplies the
registered device identity required for Linux stat. No Metadata, FileAttributes,
Node operation, Linux stat layout or syscall number changes.

The checked `Node::change_mode(mode, credentials)` boundary admits mode mutations
under maintained_noatime. It supports ordinary regular and directory inodes, root or matching
effective UID, and clears SGID for a nonroot caller with mismatched effective
GID. The current flat credential domain has no supplementary groups or fsuid.
Legacy credential-free chmod remains unsupported. Trusted update_metadata is
restricted to ordinary regular inodes under maintenance_only, so its caller-supplied ctime cannot acquire
maintained provenance. Other policy-specific operations are described below.

Update admission stays exclusive from the initial inode/permission decision
through active realtime sampling, prepare, commit, checkpoint and complete-image
revalidation. Spinlocks are not held over clock access or I/O. A validated active
boot-RTC or administrator-software sample supplies ctime; seconds outside
[0, INT32_MAX] return EOVERFLOW before conversion or mutation. The transaction's
existing ctime nanosecond and wire formats are unchanged. Missing/invalid clock
samples return EIO. Same-mode chmod still increments inode version and records
the sampled ctime, even when that sample equals the previous timestamp.

Denied ownership, unsupported type, unavailable clock and representability
failures issue no writes or flushes and preserve a healthy mount. Reader or
canonical pins prevent update admission. Transport/integrity failures quarantine
regardless of their Status spelling. The pre-prepare rejection path does not
rearm an owner that is still open. Checked and trusted updates share the existing
publication/checkpoint path and exact sealed/canonical/home byte comparisons.

The immutable policy changes the exported C++ owner layout: use SDK/module ABI30
and rebuild consumers against matching headers/archives. Stale SDK29 is not a
compatible substitute. Root format admission and native UAPI remain unchanged.

## Evidence boundaries

Hosted tests exercise the actual VFS/POSIX service over model transport, including
FD/dup/fork retention, aliases, repeated updates across inode table blocks,
reader admission, all second-operation writes/flushes/reads and initial admission
read faults. Separate private-state tests relax compiler access control to attack
capacity, epochs, partition bindings and canonical image equality. Those flags
are never production compiler settings. All-ten freestanding objects are compile
evidence, not execution evidence.

The SDK-only native fixture links exported architecture/mode artifacts and the
OS Virtio driver. It runs internal maintenance through mounted VFS/POSIX metadata
queries; it does not execute a user chmod syscall. Native fault injection reports
failure after completed emulated transport calls, followed by fresh-boot recovery.
Normal native publication/checkpoint images are checked by full-byte oracles,
offline e2fsprogs and an actual Linux guest. These are separate evidence classes.

The escaped-payload fixture deliberately places JBD2 magic in an otherwise unused
reserved inode slot at the start of a participant block. This is synthetic input
admitted by the bounded allocated-inode reader, and conventional fsck rejects the
reserved inode contents. It establishes exact escape handling and partition-guard
preservation only. It is never used as fsck-clean or Linux-mount evidence.

This provider does not qualify a general journalled writable root, allocation,
data/namespace mutation, unmount/hot removal, SMP filesystem concurrency, DMA
fencing or physical power-loss durability. The default maintenance provider
originated in SDK15; the opt-in policy and public checked-mode path require
SDK/module30. This bounded slice is progress toward M4/STORAGE_EXT4 S4, not
completion of modern root or the reader/recovery/transaction feature matrix.
New maintained-policy hosted tests use actual VFS/POSIX and Linux call/format
adapters over model transport. Their markers explicitly exclude user execution
and hardware. Native user-mode, SDK and crash/recovery qualification of the new
public path are separate required campaigns; the old maintenance fixture does
not establish them.

## Ordinary-directory mode extension

Checked chmod admits allocated directories at or above first_ordinary_inode.
Root inode 2, reserved system inodes and symlinks remain unsupported. Directory
permission changes preserve the inode type, owner/group, links, size, extents,
allocation and every directory data byte. Only permissions, ctime, inode version
and checksum fields change. Replay requires identical directory type and rejects
all directory ownership bytes under that mode-only policy.
The allocated-directory dot/parent/content checks still run before writes.

The low-level Ext4MetadataTransaction public primitive also admits these mode-only
directory updates under its exclusive-media contract; a changed directory uid or
gid is rejected before writes or flushes, following admission reads. The mounted
maintenance_only update_metadata wrapper selects a private regular-only prepare
policy. This rejects directories before canonical staging and transport mutation,
without an extra cache read that could consume persistent generation capacity.
The checked path passes its admitted original type into checkpoint verification.
The shared checkpoint path preserves and verifies
the original inode type. No object layout, vtable, public structure or UAPI changes
are introduced by this extension; SDK ABI remains 30 with new exact artifact hashes.

Path traversal still rechecks search permissions through the current mounted
metadata. Retained descriptors can query metadata and restore directory mode
without traversing the directory. Directory enumeration now honors the retained open description's readable flag,
matching Linux getdents after chmod 0000. Unreadable descriptions return EBADF;
provider metadata, directory type and listing failures remain checked. Fresh opens,
relative child resolution, fchdir and fchmodat(dirfd, ".") still require current
path permissions. This is a separately tested M2 correction exposed by directory
chmod, with no added descriptor fields or UAPI layout changes.

The directory hosted suite also runs the regular suite, public VFS/POSIX operations,
whole-image writer/recovery oracles, immutable-byte mutation rejection and all
write/flush cut prefixes. It is model transport evidence, not native execution.

## Checked ownership for ordinary accounts

`Node::change_owner(uid, gid, credentials)` defaults to unavailable. The mounted
provider implements it for strict ordinary regular files and directories under
maintained_noatime, ordered_data_noatime and ordered_data_dirty_noatime. The
POSIX `change_owner_fd` and `change_owner_at` entries use the same checked
applied/read-only/unavailable/failure outcomes as mode changes. Descriptor calls
retain the open description's actual node; path calls retain and release their
checked path reference. `AT_EMPTY_PATH` and `AT_SYMLINK_NOFOLLOW` follow the
existing mode-change path rules. Root inode 2 and symlinks are unsupported.
Credential-free legacy `chown` behavior remains unchanged.

Only effective UID zero may change ownership, including a request that preserves
both identifiers. Each `UINT32_MAX` operand independently means unchanged. An
actual UID or GID change clears setuid and setgid; otherwise all permissions are
preserved. Sticky and ordinary permission bits survive. The provider samples
active realtime for ctime, requires seconds in [0, INT32_MAX], and increments
the inode version even when the requested identifiers already match.

Ownership uses the original mounted Node slot, address-bound in-flight lease,
physical inode generation and immutable NodeMutationContext throughout prepare,
publication, checkpoint and renewal. It retains the original transaction owner,
device session and complete sealed/canonical/home image comparisons. A new
private ownership prepare policy admits directory UID/GID updates and one-inode
metadata updates while the production journal stays dirty. Checked mode changes
use the same original-node authority and a separate mode-only prepare policy,
including the production dirty mount. The public legacy
transaction and structural replay policies retain their previous restrictions.

Startup's sealed one-action route admits an explicit union of the existing
strict regular-data, checked ownership and checked mode proofs. Actual owner
differences select ownership; owner-preserving mode differences select mode;
directory and ctime-only updates select metadata before replay. Both metadata
proofs preserve size, mtime, mapping, allocation, link, generation and namespace
bytes and check bounded ctime and a single version increment. Ownership permits
only the exact privilege-bit clearing rule; mode forbids UID/GID changes.
Malformed metadata cannot fall through to the data policy. Metadata replay
requires one committed transaction, no revoked actions, and exactly one target
inode-table action for strict/dirty input or a superblock followed by one target
inode-table action for clean input.
The original descriptor/head seals, route budget and authority bookends remain
in force, with no additional transport reads for selection. Other action-count
selectors are unchanged. Complete-call interruption models cover active-log and
already-checkpointed owner and mode states; they do not establish torn-media or
physical power-loss safety.

The provider remains production `Ext4MountedMetadata<1>` with owner `<1,9>` and
no paired I/O. The added Node virtual operation requires SDK/module ABI70;
consumers must use matching exported headers and archives. Hosted actual-source
composition, authority attacks, interruption recovery and retained mode tests run
through `scripts/test_checked_ownership.py`. Debug boot includes a separately
named root-policy/provider-refusal model. These remain distinct from native
ordinary-account execution and physical validation.


### Synchronous plain orphan-capture read guards

`Ext4OrphanChainCapture::capture` and `plan_head` accept an optional constrained
`ReadDispatchGuard` reference as their final argument. The guard is borrowed for
one synchronous call, including physical bootstrap, metadata, directory and
comparison reads; no hook is retained for later planning. Compatibility calls
use the same implementation with a local pass-through guard. Retained bootstrap
images need no physical dispatch. Delegated histories forward the same Guard to
their original-media leaf without an additional plain hook.

Plain owned preparation, mounted startup preparation and empty-orphan recovery
reserve a fresh local opening/read-only route budget for each capture and each
later plan. Canonical pin misses between calls therefore precede the next budget
baseline. These scopes close on success and cannot refine or grant writer
permission. Opening reservation conservatively requires whole-route counter
headroom, including writer counters, although only read hooks are exposed.

Every plain capture binds an independent original Session anchor. A failed
transport wins over a simultaneous finish or attachment failure; successful
callback identity/counter drift rejects the operation and quarantines that
original Session. A refused begin performs no media dispatch or completion;
the rejected one-shot Capture still quarantines its borrowed original Session.
Hosted models, the kernel debug point and ten architecture compile probes cover
these contracts. Native mounted startup and active reclaim writing remain
separate acceptance gates.

### Synchronous active-history read guards

`Ext4ReclaimRecoveryBinding::inspect`/`revalidate` and
`Ext4OrphanReclaimCapture::capture`/`revalidate` accept a constrained final Guard
reference. Compatibility calls construct a local pass-through Guard. Active
ResultHistory, ComposedHistory, nested ChainCapture and stack-only before/ordinary
device views forward the caller's same Guard synchronously. Only the Binding,
reclaim Capture and ResultHistory physical Session leaves invoke begin/finish.
Retained proofs, projected substitutions, parser reads and local phase adoption
consume no external credits. No Capture, Binding, History, seal or row stores a
Guard or erased hook.

The mounted active opening route uses its existing shared opening budget across
classification, Binding, first-result proof and the composed dry route. Each
physical attempt is reserved before dispatch and finished exactly once, including
media failure. Independent component progress and named composed phase limits
remain checks on successful reads. Owner aggregate adoption updates its logical
shadow once after success and never bulk-charges the same physical reads again.
Failure returns before callback identity, completion, projection or aggregate
checks, preserving media error priority and original-anchor quarantine. Success
still requires every retained identity and exact completion equation to agree.

Standalone disk-capture checks use one fresh local read budget per synchronous
capture, ending before intervening cache pins or writer I/O. The mounted active
route closes its opening budget after dry validation and retains the unsupported
writable-gate result. Existing physical enumeration and read capacities are
unchanged. Hosted fault/lifetime models and freestanding probes cover these
contracts; native mounted media, physical durability, active writes and SMP
remain separate acceptance work.
