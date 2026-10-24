# Read-only native mount inventory

UAPI0.11.0 adds native syscall 1034 (`mount_info`). Arguments are the full native
word slot, writable output address, exact 288-byte record size, and three zero
reserved words. Linux personalities return ENOSYS. Invalid scalar arguments
return EINVAL, invalid writable ranges or failed copies EFAULT, and an active
namespace operation EBUSY. A copy fault may have stored an output prefix.

The eight uint32_t fields and 256-byte path in `ok_mount_info` use target native
byte order and the fixed offsets asserted by `native_mount_info.hpp`. No kernel
pointer or object representation is copied. Every output byte is initialized.
The existing running-caller and checked-copy runtime boundary owns marshalling.
This operation is public within the current global native namespace.

Slot 0 is the underlying RAM namespace, even when another root covers `/`.
Slots 1 through 8 are stable lifetime attachment identities. The per-call row
reports unused, borrowed provider, owned provider, owned RAM or detached history.
Detached rows retain a historical recorded path and parent but no original
provider/RAM-kind claim. Ownership state is not filesystem type, device name,
read-only state, free space, an ownership reference or durability admission.

`mount_snapshot` copies scalar/path state under the VFS lock and refuses an
active namespace operation. It makes no provider callbacks, retains or releases.
Unused rows suppress draft bytes left by a failed borrowed attachment. Output
is assigned only after a successful observation. Generation is computed from
existing permanent slot states as `1 + used + detached`, range 1 through 17.
Every published attach or detach advances it; neither slot reuse nor new stored
fields are introduced. It does not describe reference counts, set_mode, socket
owners, cleanup failure, file contents or other non-topology changes. All row
fields returned here are constant between topology generations.

An enumerator reads slots 0 through 8 with equal generation and rereads slot 0
before accepting a complete topology observation. It must bound retries. The
interface does not supply a simultaneous whole-table reference snapshot, mount
admission, drain, synchronization or seal. It leaves existing detach and clean
HALT semantics unchanged. General mount/umount mutation and SMP qualification
remain open.

The additive nonvirtual method, standalone value/encoder and syscall number do
not change an existing class layout or vtable; module ABI69 is preserved. The
SDK advances to UAPI0.11.0 and explicitly advertises the inventory profile.
Consumer admission requires its hashed profile/value/encoder headers and exact
wire declaration, constants and version directives. Installed UAPI0.10 SDKs are
not modified by candidate qualification.

Hosted actual-source VFS controls, hosted dispatch with mocked mappings,
all-twenty emitted C/C++ wire compilation, Debug kernel observation models and
ordinary native user-program guests are distinct evidence lanes. No one lane
alone establishes mount mutation, physical hardware, SMP or a full milestone.
