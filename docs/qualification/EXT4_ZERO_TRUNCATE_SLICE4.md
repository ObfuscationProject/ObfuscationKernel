# Zero-truncate journal formatting and guarded Session preview

This private source slice adds a canonical pure journal formatter and attaches
the existing complete read-only preview to an original `BlockSessions::Session`.
It does not dispatch WRITE/FLUSH, publish or checkpoint a transaction, authorize
cold recovery, enable mounted truncation, export SDK53 or publish a Node. The
caller excludes replacement and every other Session user/writer throughout the
attempt and keeps the service, raw device, Session and off-stack preview storage
alive. The guard is not an SMP exclusion lock or an atomic service reservation.

`Ext4ZeroTruncateJournalFormatter` accepts one dirty empty fixed-ring journal and
an exact four-home plan. It builds descriptor, escaped payloads, synchronous
commit, zero end, active head and prospective empty head using caller-owned
scratch. The accepted witness codec regenerates the full canonical old/after
transform before encoding. The complete journal verifier then independently
parses the generated records, restores logical payloads, decodes/regenerates the
witness and matches all-before homes and every plan field. The prospective empty
head is checked separately. Times come from the witnessed canonical after inode.
The format retains the admitted checksum-v3/revoke feature profile and excludes
the 64-bit journal feature. Both sequence increments are checked before success.
Public formatted images and reports have no transport or lifetime authority.

`Ext4ZeroTruncateSessionPreview` owns a private static Reader/Dispatch, original
Session anchors, binding/geometry/bounds/progress/control/flight seals, and the
one-shot off-stack preview/workspace. It checks the exact original Session
attempt/completion prefix before each READ and through the Session's pre/post
runtime device callbacks. A callback cannot redirect failure to a foreign
Session; failure observations use an independently anchored original pointer.
Every transport failure or retained-integrity failure quarantines that original
Session. Initial shape/capacity refusals dispatch no I/O. Public report copies
and counter computations cannot acquire a Reader or dispatch a later operation.

The full concrete preview READ budget is derived independently from the existing
checked view and ownership algorithms. Let `G=metadata_count/4`, with
`1 <= G <= 32`, and `D=2+4G`, the maximum READs for a checked inode decode:
inode bitmap/table plus four inline extent allocation checks, each bounded by G.

| Component | Conservative attempted READ capacity |
| --- | ---: |
| One strict view opening | `2 + 2D + 1024` |
| One full ownership proof | `2D + G + 128(D+1) + 2 + 2G` |
| Initial/final captured-byte reads | `13 + 13` |
| Two complete old/final views | `2866 + 1062G`, at most 36,850 |

The scan refuses record 129 before its inode transport. Group count matches the
binding before any group loop. All errors stop a prefix; overlay hits charge no
Session READs, so counting them remains conservative. The in-memory JBD2 parser,
checksum work, geometry/report queries and source-label lookup add no device
READs. BlockSessions does not retry selected transport. The whole attempted and
completed read capacity is checked against the original counters before the
first dispatch. This is only the preview's budget; later publication, checkpoint,
recovery, final view reopening and mounted cache operations need separate derived
WRITE/FLUSH/READ and sequence reservations.

The authoritative Session report and preview transport totals are distinct from
the guard's local tally. Ordinary transport faults credit their exact attempted
prefix and no failed completion. If a retained field changes in flight, the
guard refuses before crediting its local tally, which can therefore differ from
the observed original Session counters. Neither tally acknowledges persistence
or reclamation. Failed guard selection may remain recorded as pending even after
the Session itself has drained; the failed one-shot object grants no further I/O.

Hosted models preserve all sixteen whole-home mixtures, old/final ownership,
cross-group layouts, escaping, admitted revoke flags, timestamp and sequence
limits, forged plan fields, input aliases, whole-counter boundaries, every READ
failure position of the exercised baseline and single-field retained-state
changes at both runtime boundaries. Model device callbacks and kernel Session
bookkeeping remain separate from native driver execution and durable media.
Generated large media exercises 32 initialized groups, 128 allocated records,
1,024 root-directory blocks and a different inode with four initialized extents
in four other groups. All sixteen home mixtures require 3,032 Session READs,
including 2,048 directory block READs over the two views. An independently valid
allocated-empty record 129 refuses with ENOMEM after 1,421 completed READs;
its distinct table block is never dispatched. These executions challenge the
algorithm's capacity edges without asserting that the conservative maximum
36,850-read bound is reachable or that generated media has Linux provenance.
Individual host compiler stack frames and hosted ABI51 layout comparisons do not
qualify ten-profile native layouts or complete native call chains.

Typed publication/checkpoint/recovery, inode-before-allocation durable barriers,
mounted Owner/lifetime/cache integration, generalized allocated-empty rewrite,
both startup families, native ten-profile SDK/OS consumer qualification,
four-musl direct operation/cold restart and independent Linux media/replay
inspection remain required before enablement. Full M4 and M0-M12 remain open.
