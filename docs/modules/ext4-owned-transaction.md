# Owned ext4 metadata transaction and checkpoint

`Ext4OwnedTransaction<DeviceCapacity>` implements one checked ordinary-inode
metadata update and its checkpoint in a private `MetadataBuffers<3,1024>` session.
The admitted profile remains one two-action CRC32C-v3 journal transaction on a
1 KiB ext4 volume with 512-byte sectors. It grants no mount capability, file data,
directory, allocation or generic writeback API.

Open with an authoritative BlockSessions registration and physical partition
bounds. Engine offsets are relative to the private session. `prepare(update)`
validates and reserves all three canonical/private versions without a write or
flush. `commit()` emits seven writes and five flushes, then publishes the exact
checked logical images. `checkpoint()` emits four writes and four flushes, then
publishes clean controls and retires the retained journal dependencies.

Readers pin immutable canonical versions. Preparation leaves old bytes visible.
Commit and checkpoint require every external pin and pending reader admission to
drain; a busy return occurs before device work and can be retried. No new reader
is admitted during these operations. After commit, the canonical inode reflects
the durable journal while its home block remains old. All three versions remain
pinned until checkpoint completes. Slot generations reserve both publication and
cleanup advances before the first flush. No lock spans device I/O.

Only a successful invocation of this owner's writer activates its private
noncopyable authority, bound to owner/cache/session identity, registration and
transaction generations, participant slots, sequence and exact sealed images.
Neither reports nor a caller-provided status can authorize publication.

`discard()` retires an uncommitted prepared transaction without I/O. `close()`
requires reader drainage and an uncommitted or checkpointed state. Local input,
unsupported profile and capacity rejection close cleanly. Integrity or transport
failure aborts both cache and session; retained pins cannot read failed canonical
state. An ambiguous publication or failed checkpoint retains quarantine, private
versions and journal dependencies and permits no in-place retry or reuse.
Fresh boot or externally fenced recovery decides the actual durable state.

Owner, registry and transport backing must outlive every handle and admitted
call. Destruction is not fencing and cannot make a quarantined registration
reusable. Bind handles before synchronized publication; destruction must not
race calls. Synchronous device success still relies on actual transport FLUSH
semantics; hosted fault models do not establish physical power-loss behavior.

Hosted tests keep writer, completion, cache and session regressions separate.
`test_ext4_owned.py` captures fault/visibility/concurrency cases and all-ten
freestanding compiles. The separate invariant binary uses compiler access-control
relaxation solely to corrupt private state in hosted tests; production headers
expose no corresponding access. SDK-only native probes exercise actual Virtio
transactions, and Linux interoperability boots copies of the exact generated
publication and checkpoint images rather than substituting an oracle disk.

Direct orphan readback, standalone terminal inspection and post-opening mounted
reads use a local `Ext4DirectReadScope`. Each scope retains the original Session,
its geometry and exact prefix, and a separately captured paused Owner shadow.
One successful physical read is verified before the complete dispatcher returns;
the shadow advances once only after external finish hooks also succeed. Routine
pre-dispatch begin, capacity and shape refusal leaves the Session and shadow
unchanged. Finish refusal or integrity rejection leaves the shadow unadopted and
quarantines the original Session. Media failure keeps priority over finish-hook
failure. A successful
readback whose bytes differ still retains its completed read accounting.

The scope requires only one read attempt and completion credit. Balanced exhausted
write/flush counters do not block it. Terminal inspection preserves nonempty
sector-aligned transfers, including odd first sectors, within the original
Session bounds and transfer limit. Opening reads keep their original route Budget;
post-opening reads never reuse the closed Budget. The scope, authority checker
and hooks borrow storage only for the serialized call and cannot be copied or
retained across Owner reset. This adds no writer phase or active-reclaim admission.
