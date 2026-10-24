# Mounted cross-parent regular-file move

This private successor connects the distinct regular-move writer and strict
completion to the original mounted metadata owner. The finite VFS operation
accepts an ordinary regular file, two different directories in the same mount,
and an absent destination. Replacement and cross-parent directory move remain
outside this operation. Existing same-parent file/directory rename retains its
separate qualified implementation.

The source uses the original address-bound Node mutation role. Both parents
retain separate original I/O leases across physical generation checks, writer
preparation, journal publication, strict checkpoint, canonical cache/disk
comparison, binding renewal and name publication. No public inode key, report,
copied Node or copied lease supplies mutation authority. All three checked
generations and the original publication epoch must remain exact.

Before any mutation, immutable name-history capacity is reserved. Completion
must physically verify its whole after images and durable empty journal head.
The mounted continuation independently compares canonical cache images with
the actual Session, reopens the checked view, verifies namespace and file
identity, and rearms the original owner. It appends the new immutable name only
after those checks. Retained file descriptions and previous name views remain
valid. Failure quarantines the original session and does not publish a name.

Cold startup selects the explicit `OKMVW001` witness before any legacy fallback.
Its separate bounded cursor keeps the original mounted owner/cache/Session
through recovery and ordinary root admission. All actual cold READ/WRITE/FLUSH
operations remain guarded by that cursor. The strict proof retains the
independently empty orphan chain; it grants no namespace-replacement authority.

The authored hosted model covers mounted healthy movement, lifetime holds
during actual journal I/O, descriptor release during movement, rejection of a
concurrent operation/drain, duplicate-target and invalid-name refusal, immutable
name views, payload identity, populated-parent rename and strict cold startup
after publication FLUSH reports failure following persistence. A foreign marker
with a valid descriptor checksum must fail before any write or FLUSH.

Source preparation and hosted acceptance remain separate. Hosted transport
faults are synthetic. Native SDK-only consumer qualification, real user-mode
Files/Notes actions, hardware acceptance and full M0-M12 completion require
their own results. The installed default SDK and preserved successful commands
remain unchanged while this private successor is being qualified.
