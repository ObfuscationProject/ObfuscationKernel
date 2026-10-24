# Owned mount publication and resource drainage

This experimental SDK50 interface changes exported C++ layouts and requires native
module ABI49. Linux syscall/UAPI layouts remain unchanged. It preserves all ten
Kernel profiles and the distribution's four supplied musl userlands.

`Ext4MountedMetadata::retain_root()` issues an exact, noncopyable publication
reference. Move construction relocates its live record and clears the source;
`clone()` creates a separately counted reference. `release()` consumes the caller's
token even when integrity checking fails, preserving the issuer's original failure
and internal record. The concrete domain holds at most 32 references and never
wraps its serial. It does not allocate or extend a provider's C++ storage lifetime.
Provider, Registry, transport and workspaces must outlive every token, node retain,
covered path, descriptor and cwd reference.

`VirtualFileSystem::mount_owned(path, token)` transfers a reference by value and
consumes it on failed admission. Its attachment owns both the root reference and
the actual retained covered path. `VfsMountHandle` is a copyable attachment
identity; copying it acquires no ownership. `detach(handle)` commits namespace
removal once and then attempts fallible cleanup. A cleanup error never republishes
the mount. Child mounts must detach first, aliases hold independent references,
and detached slots/IDs remain permanent tombstones. The bounded table supports
eight lifetime attachments; exhaustion requires a new VFS instance. Generation
bearing reusable IDs are future work. Stale context IDs return `not_found` before
any stored node pointer is dereferenced.

`mount_borrowed(path, node)` and the compatibility spelling `mount(path, node)`
retain their external storage contract. Raw mounted-provider root or lookup
exposure permanently bars qualified close/seal; there is no guessed raw-alias
release. Borrowed publication invokes the provider's exposure hook before
commit, including a node obtained through a retained path. A failed hook or
temporary root release prevents publication; the original operation error has
priority over cleanup errors. An unsupported hook keeps the provider's explicit
external-lifetime contract. Borrowed mounts cannot be retired through owned
detachment.

After all root attachments detach, `begin_drain()` closes provider admission in
one direction. Existing inode/descriptor/cwd obligations can release; new retains
and I/O are refused. Pending orphan work must complete beforehand. Final
`freeze_clean()`/`close()` requires the exact root and node records to be drained.
A private no-I/O Owner preflight checks its actual pending operations before
drain and again before seal; a mounted summary or erased deferred pointer cannot
hide outstanding cleanup.
A busy seal performs no seal I/O and remains draining. Integrity or media failure
quarantines the original owner and cannot produce a clean-shutdown claim.

`SchedulerState::drain_resources()` consumes every fd/cwd edge, clears the consumed
pointer and mount ID, attempts all cleanup, and preserves the first failure.
`drain_all_resources()` retains a sticky report for an explicit serialized shutdown
coordinator. Process exit/wait status is independent of this cleanup diagnostic.
The containing Kernel declares backing filesystem/UDP storage before Scheduler
and drains resources in its destructor body while those backings still exist.
Destructor fallback cannot report or manufacture a clean volume.

ObfuscationOS publishes its journalled root through the exported retained API and
stores its attachment identity next to static provider storage. It forbids
placement reconstruction after the first construction. The legacy nonjournalled
branch remains borrowed. The distribution still must implement service admission,
application quiescence and fallible power preparation before committing halt or
reboot. These mechanisms alone do not complete desktop shutdown, SMP, Linux
`umount`, dynamic filesystem unloading, physical M11 or manual M12 acceptance.

Debug point66 adds an explicitly labelled refusal model: invalid publication
references/attachments, unused provider rejection, pre-dereference stale-context
checks, and empty Scheduler drainage. Positive mounted-media lifecycle, durability,
interruption and SDK-only native execution require separate evidence.
