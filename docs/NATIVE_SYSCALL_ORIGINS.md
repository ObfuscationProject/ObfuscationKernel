# Owned native syscall origins

The runtime keeps bounded private syscall-origin records beside its 128 entry
stack leases. These records are a prerequisite for later scalar pipe interruption
and restart. This implementation does not admit signals to blocked pipe readers,
publish restart frames, or alter the existing sleep-only interruption policy.

`src/user/native_syscall_origin.hpp` defines `SyscallOriginTable<Profile>`.
Architecture policy uses the separately reviewed static `RestartAdapter<Profile>`.
There are no new fields in UserFrame, Trap, TCB, ProcessResources or exported
readiness layouts. `KernelEntryStackPool::active_epoch` adds a layout-neutral
validation operation. ABI29 and all ten architecture profiles remain intact.

## Capture boundary

`SyscallOriginTable::enter(process, thread, view, scheduler)` calls the actual
`arch::execution::enter_owned` itself. There is no minting API that takes a
caller-supplied Trap. Before and after entry it validates the canonical scheduler
owner and the pool's private record pointer, slot, lease generation, activation
epoch and PID/TID. Matching owner numbers and generations from a different pool
are insufficient. A stale copied view fails even when its slot has been activated
again for the same owner.

The returned privileged capture must lie in that same active view. Linux scalar
read input registers, the complete original frame, exact returned trap and copied
opcode are retained before result encoding or view deactivation. The full two- or
four-byte opcode interval must have executable mappings and pass checked user
copy, including a crossing second page. The opcode is never re-read at handler
selection. Subsequent unmapping or rewriting does not mint another origin.

The existing private n64/PPC backend observations are consumed before the view is
deactivated. Their failure revokes the provisional unbound record before dispatch.
Only actual pipe arm can bind it. Timer, initial/child state, non-read calls,
unsupported opcode/trap/mode and MIPS Cause.BD obtain no restart authority. Normal
unsupported-origin syscall completion is preserved; this is not an EINTR fallback.
Checked image and capture counters never wrap. Exhaustion fails closed.

## Wait and lifetime ownership

Initial blocking read binds the actual retained object handle, exact scheduler
WaitTicket, initial request generation, original numeric fd, output/count and
retained description/pipe IDs. A descriptor replacement cannot redirect this
binding. Spurious readiness rearm keeps the capture, image and object identities
and advances only the exact ticket. Previous permits then fail validation.

Permit validation re-resolves canonical process/thread storage by stable IDs from
the captured scheduler. It checks current lease, image, address space, the full
unmodified syscall-input frame, pending exact wait and readable original pipe
description. A cancelled/replaced wait is rejected before the prune sweep. No
movable PCB, TCB or frame pointer is retained, so process compaction needs no
origin pointer rebasing. Existing n64/PPC readiness relocation transactions remain
unchanged and independently required.

Ordinary completion, pipe terminal cleanup, explicit thread retirement and group
release revoke authority. Preparation gives each new lease a new image identity;
fork/clone copies no parent origin. Committed exec retirement revokes all old
process captures, and install assigns the replacement image a new generation.
Returnable failed exec does neither. New entry retires any previous unbound
capture before calling the backend; a restored sigreturn frame is never itself
treated as an original trapped read.

Shared-VM replacement first stages every live origin tied to the old address
space. All canonical owners and captured frames must validate before the old
page tables are destroyed. A staging or table-retirement failure changes no
origin. After resources are published, commit revalidates every staged capture
before changing any stored address-space identity. One stale participant rejects
the entire origin update. The capture/image/opcode/wait identities are retained;
old space-bound permits become stale. A post-publication failure stops runtime
execution instead of allowing a partially authorized user return.

## Typed publication interface

`Identity` carries `lease{pool,slot,generation}`, `pid`, `tid`, `image`, `capture`,
`epoch`, `lower`, `top` and `space{root,slot,generation}`. The runtime obtains the
current key with `identity(process, thread)` and constructs `PipeBinding` from
the retained `PipeReadWait` via `pipe_origin_binding`.

`prepare_restart(identity, current_frame, binding)` returns an opaque
`RestartPermit`. A default permit is invalid. Only the table constructs a valid
permit; its immutable accessors expose `identity()`, `binding()`, `capture()`,
`trap()` and `plan()`. The table recomputes the pure plan from its retained input
frame/opcode and binds the permit to that exact issuing table. A plain Plan,
synthetic Trap, user-edited signal frame or copied public identity is not a permit.

`validate_restart(permit, identity, current_frame, binding)` rechecks live canonical
ownership without mutation. `consume_restart` takes the same arguments, validates
again and revokes the source authority exactly once. Later typed n64/PPC
publication must stage against the unchanged captured frame and consume before
changing the canonical frame. Wait release and handler construction follow within
the same serialized service boundary. No such publication or signal admission is
enabled by this prerequisite alone.

## Verification boundary

`scripts/test_native_syscall_origin.py` runs real runtime/scheduler/page/object
code with hosted trap/copy/mapping backends across all ten selected ABI profiles.
Its controlled synthetic `enter_owned` is explicitly a test double. Tests cover
full executable-copy spans, equal-number foreign pools, stale activation views,
generation exhaustion, literal target syscall encodings, MIPS BD refusal, exact
fd/pin/wait identity, copied opcodes, ordinary completion, rearm, failed/committed
exec, child preparation, slot retirement, compaction and multi-owner transactional
space replacement. These tests do not execute target instructions, signal
handlers, typed n64/PPC publication, isolated user address spaces or SMP.

Freestanding target compilation of actual runtime in ordinary/private signal
modes is retained separately. It is not a standalone kernel link, exported SDK,
OS guest run, physical-machine acceptance or complete syscall restart support.
