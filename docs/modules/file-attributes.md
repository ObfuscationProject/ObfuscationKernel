# Checked file identity and timestamp provenance

`fs::FileAttributes` extends checked native service metadata with an explicit
validity mask. An unavailable field has no asserted value. It is not a successful
zero inode, device or epoch. SDK/module ABI14 carries the new C++ return layout;
the native UAPI0.4.0 `ok_stat` record and its copyout size remain unchanged through
an explicit field projection. Native Linux stat-family wire codecs remain disabled.

Every VFS assigns nonzero monotonically allocated RAM inode numbers, including
archive-backed RAM nodes. Hard links, renames and retained unlinked nodes keep the
number. Reusing a node slot consumes a new number. Archive replacement of the same
closed, unshared inode keeps its identity. Unowned standalone nodes have none.
Sequence exhaustion is checked before object publication and never wraps.

A bounded VFS-owned registry assigns major0/nonzero anonymous minor pairs to live
provider instances. The internal `filesystem_identity()` token is only a lookup
key; no exported number is derived from its address. Aliases of one provider share
the device pair within that VFS; distinct registered providers differ. Registrations
are retained for the entire VFS lifetime, including covered roots and retained
paths/descriptions. The provider backing must outlive the VFS and all its retained
references and must not be reconstructed at a registered address. Numbers are
namespace-local, not persistent disk IDs or physical major/minor numbers. Independent
raw mounts of the same disk are not qualified aliases. Existing mount/path lifetime
rules remain in force; this change adds no unmount or transport ownership API.

The admitted nonjournalled ext4 provider reports its actual disk inode number.
A clean read-only mount can also report stored atime, mtime and ctime with
`readonly_ext4` provenance. The decoder uses signed base seconds plus the two epoch
extension bits, validates each field against `i_extra_isize`, and rejects malformed
nanoseconds or extents without changing the output. It supports 128/256-byte inodes
and preserves negative epochs, actual epoch zero and dates beyond 2038. Device
identity is attached by checked VFS/POSIX composition, not fabricated by a Node.

The default writable ext4 policy does not maintain timestamps; its times are unavailable
from mount time, including before the first mutation. SDK20 adds an explicit
opt-in [maintained policy](../MAINTAINED_EXT4_TIMESTAMPS.md), qualified by separate
event/fault, native/musl and Linux interoperability gates. RAM and archive-v1 have no
epoch source and also leave times unavailable. Pipe and console records remain
explicitly incomplete because they have no qualified identity providers in this
slice. Existing native block accounting remains unchanged and is not thereby
qualified as Linux `st_blocks`.

The checked active realtime service and administrative software selection are
documented in [REALTIME_SETTING.md](../REALTIME_SETTING.md). Availability of that
clock does not itself qualify a provider's event history. musl can consume statx BASIC fields without
checking the validity mask, so partial metadata does not authorize successful
Linux stat-family responses.

`scripts/test_file_attributes.py` exercises hosted VFS/POSIX identity lifetimes and
codec boundary checks for all ten profiles. `scripts/test_ext4_attributes.py`
compares raw inode integer oracles and debugfs output on 1/2/4 KiB images with both
inode widths, checks read-only image hashes and writable timestamp unavailability,
and runs offline fsck. Kernel debug tests check codec/registry behavior and all four
native stat-table projection boundaries. The separate OS
`tools/jbd2/test_file_attributes.py` consumes an exact exported SDK and boots actual
Virtio-backed probes. Native service dup/fork checks are distinct from user-mode
Linux/musl calls, native SMP and physical hardware claims.
