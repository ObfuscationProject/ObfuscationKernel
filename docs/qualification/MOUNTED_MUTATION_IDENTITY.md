# Authentic Node admission for mounted write and truncate

Mounted regular-file write and truncate use an actual provider-owned Node address
and its complete mount, original Session, inode and generation key. Admission
checks the original populated slot and actual lifetime record under the mount
lock, obtains a noncopyable address-bound I/O Lease, and closes mutation admission
in that same critical section. A copied Node, foreign Node or altered key cannot
substitute for the original slot.

A noncopyable, nonmovable synchronous operation context retains the original
Node reference, complete key, actual Lease reference and admission epoch. The
admitted write/truncate helpers explicitly carry that context through rejection,
every commit identity boundary and terminal publication. Mutable mount binding
fields are checked against this original context; clearing all of them cannot
erase the operation or make its terminal path reopen admission prematurely.

The synchronous wrapper retains its actual I/O Lease through checked generation,
preparation, WAL publication, checkpoint, canonical image comparison, renewed
reader validation and Owner rearm. Resident cached inode bytes do not supply a
new physical observation: a separate original-Session inode-table READ verifies
the complete inode checksum and generation. Prepared and renewed inode images
must retain the admitted generation as well. Physical observations belong to the
mounted prefix or suffix and must be reserved separately before any future typed
child owns exact Session counters.

Unprepared rejection and successful checkpoint leave admission closed until the
wrapper releases the actual local Lease and clears its private binding. Failed
paths release that Lease while preserving provider quarantine. Hold release
performs no transport I/O under the mount/lifetime locks. Deferred orphan cleanup
runs after lock release. Existing descriptor release during updating is allowed;
the operation's I/O hold prevents inode retirement until the operation finishes.

Zero-byte writes retain the existing no-op policy while checking the authentic
Node in a qualified read operation, holding an actual I/O Lease and checking its
physical generation. Same-size truncate also checks generation and releases its
hold without changing the admission epoch or issuing a WRITE/FLUSH.

The hosted model verifies actual source paths and includes copied/foreign/key
controls, resident-cache physical generation drift, real lease exhaustion,
descriptor release and blocked reentry from runtime callbacks, and transport
failure cleanup. These are hosted provider models. They supply no native target
call-chain, Kernel SDK export/installation, SMP or physical durability admission.

The author predecessor and its reproduced combined-binding-erasure defect are
preserved separately. The repaired model additionally covers ten single, combined
erased, foreign or same-shape decoy bindings at six actual operation phases for
both write and truncate. Prefix/prepare refusals issue no WRITE/FLUSH; later
uncertainty quarantines the provider while retaining and then cleaning up the
original local I/O Lease. No callback observes readable admission before release.

Public admission additionally checks an exclusive mutation role on the actual
occupied lifetime I/O slot. This provider-only role seals the original Lease
object address, context address, full record key and epoch; release of that real
Lease consumes it. The gate scans these actual slots before admitting public
Node execution, root issuance, another update or drain. Restoring a diagnostic
readable phase, erasing all mutable mount bindings, or clearing summary I/O
counts cannot erase the occupied role. Ordinary reader holds have no mutation
role, so qualified retained lookup still acquires its legitimate descriptor.

Generic release refuses a claimed mutation role, and move construction refuses
transfer of that role's original token while preserving the original token and
returning an invalid destination. Only the provider-private original-context,
key, epoch and Lease terminal release consumes a claim. Ordinary unclaimed Lease
move/release semantics remain available. The author preserves the v3 actual
generic/moved-token release reproduction and adds twelve operation/stage release
controls plus an ordinary transfer positive control.

Interstage identity requires updating with no admitted reader. The physical
prefix is followed by an original-context identity check. Descriptor release
remains legal and pending orphan cleanup performs no transport until the role
is consumed. Hosted controls cover seven diagnostic/binding/count variants at
six actual stages for both operations, including permanent phase drift. These
are serialized defensive controls against accidental state drift, without a
claim of protection against arbitrary hostile kernel-memory modification.
