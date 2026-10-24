# Guarded allocated-empty Session preview

This design prepares a source-only read adapter. Implementation follows exact
independent acceptance of allocated-empty preview source-review-attempt-02 and
the selected original-Session observation repair. Frozen inputs and failed
attempts remain immutable. The adapter does not enable a mounted writer.

## Retained authority and lifetime

`Ext4AllocatedEmptyDataSessionPreview<Session>` is noncopyable and nonmovable.
It owns an off-stack read-only preview, its permanently claimed workspace, a
guarded reader and runtime dispatch guard. It retains the exact original
Session once through `reclaim_history::SessionAnchor`, together with the complete
filesystem binding, volume geometry, partition bounds and exact initial report.
Public report copies contain diagnostic facts and cannot authorize writes,
recovery, checkpoint, cache publication or another preview attempt.

The owner must exclude other Session users, mounted writers and raw-device
aliases and retain the registry, device, Session and object storage throughout
the serialized attempt. Reachable device backing storage must be disjoint from
mutable preview storage. Counter and image seals are integrity checks; they do
not implement a writer lock, pin a disappearing device or establish SMP safety.

Object/input overlap refusal occurs before modifying any potentially aliased
byte, claiming workspace or dispatching a read. A structurally valid attempt is
permanently one-shot, including failure. A later transaction obtains private
captured state through an explicit friend boundary; it never trusts returned
reports as handoff authority. No such transaction integration is enabled here.

## Complete READ reservation

Let `G` be the checked metadata group count, between one and the inherited group
capacity. A strict checked view plus full global ownership has an upper bound
`A = 1420 + 531G` physical reads. The old allocated-empty validator adds three
source reads and five metadata bookends. The final ownership uses the existing
global proof. Capturing and rechecking eight journal records, four physical
homes, the unchanged inode bitmap and the data block adds 28 reads. Therefore
the conservative whole preview bound is `2A + 8 + 28 = 2876 + 1062G`.
Private overlay hits cause no Session read; including them in `A` preserves the
upper bound. Scalar parsing and canonical formatting perform no device I/O.

Before the first read, require a stable owned Session with nonzero identity,
registration generation and serial, no first error, no in-flight operation or
pending writes, and matching attempted/completed READ, WRITE and FLUSH totals.
Check both attempted and completed READ capacities independently, using checked
subtraction. Counter exhaustion, malformed metadata or unsupported geometry
must fail without transport dispatch. Reserve zero WRITE and FLUSH operations.

Require 512-byte sectors, a transfer limit of at least 1024 bytes, a valid
partition bound and a 1024-byte filesystem. The filesystem must fit inside the
Session-relative volume at its retained first-sector offset. The retained block
number selects `first_sector + 2 * block`; only the Session applies the raw
partition origin. Reject overflow and out-of-range selections before dispatch.

## Runtime and failure protocol

Check retained original pointer/anchor agreement, binding, geometry, bounds,
capacity, control state and exact progress before every read. Select one block,
retain the exact pre-read report and mark dispatch pending. Use only
`read_blocks_guarded` with the adapter's private runtime guard. At each runtime
callback require that the callback references the same anchored Session and
that its report contains exactly one attempted in-flight READ beyond the saved
prefix. WRITE and FLUSH totals, pending writes and all identity fields stay
unchanged. Recheck the original after the synchronous request returns.

An exact successful return increments local attempted and completed counters.
An exact failed return increments only attempted, records the drained original
quarantine and clears dispatch pending without completion credit. Retained
state drift or callback substitution fails through the independently anchored
original: quarantine and final observation must never follow a substituted
pointer. Original transport counters and the drained final report remain
authoritative if integrity failure prevents local tally reconstruction.

Success requires a final idle check and exact parity between local counters,
the read-only preview's counters and the original Session delta. The data block
is observed for byte consistency only. There is no data checksum, successful
data FLUSH, failed-FLUSH durability or recoverable user-data claim.

## Source acceptance

Preserve all existing allocated-empty planner, witness, ownership, journal and
preview tests. Add real hosted BlockSessions controls for all sixteen home
mixtures, one/two groups and cross-group allocation, distinct Session partition
origins and filesystem first-sector offsets, and both observed data contents.
Inject exact and partial-copy failure at every exercised READ. Require no raw
WRITE or FLUSH and unchanged complete media for success and every failure.

Exercise counter-capacity edges independently, stable-report refusals, one-shot
reuse and overlap before mutation, malformed journal rejection before home
dispatch, changed physical bookends, every retained field at runtime callbacks,
foreign Session substitution and exact drained original observations. Record
zero foreign report or transport changes. Run actual-input-bound xmake builds,
ordinary and ASan/UBSan controls and explicit freestanding template instantiation.
Freeze complete source, modes, deltas, command logs and failures for independent
review. Target full-call-chain qualification, SDK export, OS consumer builds,
guest execution, mounted integration and installation remain separate gates.
