# Private zero-truncate transaction

`Ext4ZeroTruncateTransaction<Session>` adds typed publication, checkpoint and
recovery to the independently reviewed zero-truncate preview and original-Session
observation code. This source slice is not connected to mounted mutation. It does
not enable a syscall, export an SDK, install an image or qualify native execution.

The transaction is noncopyable and nonmovable. Its only public value accessor is
an inert report copy. Production preparation, publication and checkpoint methods
are private to `Ext4OwnedTransaction`. Hosted tests deliberately expose private
members; neither that access nor a report copy grants production authority.

The owning caller must retain the Session, registration service, transport,
backing media and transaction storage through completion or quarantine. It must
exclude other Session users and raw aliases. Transaction storage is off-stack;
this slice contains no allocation or lifetime/cache/Node publication policy.

## Original Session and physical prefixes

Before the first READ, preparation validates the binding and geometry and reserves
the complete selected route's attempted/completed counter capacity. It retains
an original Session anchor separately from its dispatch pointer. The request,
binding, geometry, bounds, original report, progress, control and complete image
history have explicit comparison copies. Runtime dispatch uses static guards to
check the exact in-flight original report before and after the transport callback.
Only a successful exact return advances a local physical prefix.

A failure quarantines the agreed original and observes its authoritative report.
The observation helper does not synchronously drain an in-flight request: the
Session I/O finalizer drains that request before the outer return. Failure reports
after return contain the original's drained observation. An exact failed I/O
dispatched by the transaction records one local attempt without completion or
physical-prefix credit. A failed delegated preview has no successful private
handoff, so its counters are not imported into local transaction tallies. That
failure or an integrity failure can leave local transaction tallies incomplete;
the report does not claim otherwise.
Foreign Session pointers and media never become fallback authority.

These comparisons detect the tested retained-field changes. They are not a
general RAM-corruption defense or a substitute for excluding concurrent writers.
Before minting, a capacity/profile refusal performs no media I/O and leaves the
original Session report unchanged. An unfinished minted transaction quarantines
its agreed original when destroyed.

## Preparation and publication

Fresh preparation opens a strict old view and proves complete ownership. It
captures eight WAL records, four home blocks and the inode bitmap, constructs the
canonical plan and formatted WAL, and runs a full accepted preview through a
generated journal overlay. The preview must reconstruct the exact private plan.
All thirteen physical participant images are compared again with the original.

Publication writes the descriptor, four payloads and zero end record, then flushes
and compares the thirteen images. It next writes the commit, flushes and compares,
then writes the active head, flushes and compares. Home blocks remain unchanged
through active-head publication. Stale unused journal records remain exact until
their own selected successful write.

Recovery uses the exact private Session preview and validates its retained handoff
and original READ delta. It captures the complete logical plan, WAL and actual
home mask. It derives the predecessor empty head and reruns the canonical
formatter; every regenerated active WAL image must be byte-equal. All sixteen
whole-home before/after mixtures are accepted by the model.

## Checkpoint and no-op

Checkpoint first compares all thirteen physical images. It writes the missing
inode after-image, obtains an inode barrier even when the inode is already after,
and compares the physical prefix. It writes missing bitmap, descriptor and
superblock after-images, flushes and compares. It finally writes the empty head,
flushes and compares. A newly opened strict physical final view must prove the
same allocated-empty inode identity and complete ownership, followed by another
physical comparison. Allocation-after images are not newly written before the
inode barrier; no already-after recovery home is rewritten.

An already-empty file takes the no-op route. It still requires strict opening,
complete ownership, captured identity and exact empty-head checks, plus physical
readback. It performs no WRITE or FLUSH and changes no inode version, timestamp
or journal sequence. A maximum full 64-bit inode version and maximum sequence
therefore do not prevent a canonical no-op. READ capacity is still required.

## Whole-route bounds

For `G` groups, let `A = 1420 + 531G` bound one strict opening plus complete
ownership proof, and `B = 2866 + 1062G` bound the accepted active preview including
its captures. Each complete participant comparison reads thirteen images.

| Route | READ reservation | WRITE reservation | FLUSH reservation |
| --- | --- | --- | --- |
| Fresh | `B + 2A + 13 + 10*13 = 5849 + 2124G` | 13 | 6 |
| Recovery | `B + A + 5*13 = 4351 + 1593G` | 5 | 3 |
| No-op | `A + 13 + 13 = 1446 + 531G` | 0 | 0 |

At the existing 32-group limit these READ bounds are 73,817, 55,327 and 18,438.
The initial design proposed fresh `5836 + 2124G`; the implemented route reserves
thirteen additional reads for a separate comparison on entry to checkpoint. The
preparation design record preserves that earlier proposal.

Bounds apply independently to attempted and completed counters. Exact remaining
capacity is accepted; one fewer available operation is refused before READ.
Fresh reserves the active and next sequence without crossing the formatter's
sequence limit. Recovery reserves its next sequence. No-op preserves its sequence.

## Hosted evidence and remaining gates

The xmake model and explicit template-instantiation target use C++23,
freestanding flags and warnings as errors. Ordinary and ASan/UBSan runs cover
same/cross-group fixtures, raw partition origins, one to sixteen released blocks,
all sixteen recovery masks, every exercised fresh/recovery READ/WRITE/FLUSH fault
position, retained-field changes at transport callbacks, pure budget boundaries,
initial profile/capacity refusals, inert reports, binding alias refusal and no-op
version/sequence/counter extremes. Models compare nonparticipants byte-exact and
inspect successful flush snapshots. Existing preview, ownership, witness,
formatter and first/next-data planner regressions remain separate targets.

Hosted flush snapshots are not a hardware durability experiment. Hosted object
size/alignment, parent/current layout comparison and individual compiler stack
usage records do not qualify a native entry-to-return stack chain. Independent
source acceptance, mounted ownership/cache publication, target/native stack
qualification, standalone-kernel boot and OS SDK-consumer integration remain
separate gates before production enablement. Ten Kernel and four OS architecture
profiles are preserved.
