# Private namespace cold recovery admission

`Ext4NamespaceRecoveryAdmission` is a one-shot, read-only admission object for
the bounded SDK49 namespace journal profile. Its caller must own an exclusive,
stable device session and exclude other writers and caches for the object's
lifetime. It accepts partition-relative bounds when passed a `BlockSessions`
session. It does not write, flush, mount, checkpoint, or authorize orphan reclaim.

Admission reads the raw 32 MiB ext4 superblock, group-zero descriptor, journal
inode and active JBD2 record under explicit sector bounds. This first mapping is
provisional. The durable descriptor witness reconstructs each whole before
home and the old empty journal head. A private read-only device overlays those
exact images, including each half-sector, then runs the unchanged
`Ext4RecoveryBinding::inspect`. Its ordinary zero-orphan rule still applies to
the reconstructed old state. The resulting validated old UUID, journal mapping,
geometry and sequence must agree with the provisional mapping. Finally,
`Ext4NamespaceColdReplayVerifier` checks the committed record, exact target
order, every before/after home, and all current home images. A malformed
marked transaction fails terminally; callers must not reinterpret it as an
ordinary structural or rename log.

On success, `info()` exposes the active binding facts, `old_binding()` the
validated empty-log facts, `result()` the cold proof, and `scratch()` the
canonical before/after images and old head. `journal_head()`, `target()`,
`before()`, `after()`, and `current()` expose immutable images while the admission
object and owner session remain alive. `read_old_sectors()` serves the same
read-only overlay for diagnostic and integration checks; it cannot write or
flush. The owner still has to bind session identity, registration generation,
cache slots and image generations before granting checkpoint authority.

The parser's next sequence is active + 1. The qualified Linux/e2fsprogs
cleanup convention stores active + 2 in the empty journal head. The API reports
these separately as `parsed_next_sequence()` and `linux_cleanup_sequence()`.

The hosted session test publishes a private journal, admits all 32 mixed-home
states for replacement and unlink, checks 512-byte overlay halves, exercises
real `BlockSessions` partition origins, and rejects damaged heads, descriptors,
payloads, homes, UUIDs, journal mappings, bounds and read errors without any
write or FLUSH. The one-shot qualification script also runs ASan/UBSan and
compiles objects for all ten Kernel architecture profiles. This is a hosted
model and freestanding compile result, not QEMU, hardware, mounted integration
or M0–M12 acceptance.
