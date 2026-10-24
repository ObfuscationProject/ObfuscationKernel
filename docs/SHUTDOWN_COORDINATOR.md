# Shutdown coordinator candidate

This editable successor connects the bounded mechanism to the native runtime,
original OS storage owner and an x86_64 terminal halt backend. It has no power
UAPI or service-control command yet. Its internal adapters require guest clean
halt and restart qualification before promotion. Installed SDKs remain unchanged.
Every complete M0-M12 gate remains open.

`ShutdownCoordinator<Services>` uses static dispatch and fixed storage. Its
adapter is a trusted Kernel runtime owner and must outlive the coordinator. The
original native PID1 identity binds once to a runtime address, nonrepeating
runtime incarnation, process ID and original leader TID. The runtime adapter
must establish native personality, actual caller/scheduler identity, a live
single-threaded group and all six root IDs; callers cannot supply those facts
through UAPI. Incarnations must not repeat while any ticket can exist, including
reconstruction at the same address. Tickets bind this identity, coordinator,
generation and immutable action. No reset or cancellation reopens a lifetime.

PREPARE performs read-only capability and exact-owner admission before services
retire. Unsupported preflight returns to idle without publishing an action. A
repeat PREPARE for the prepared action returns the same ticket; conflicts fail.
COMMIT reauthenticates the caller and checks actual service/child quiescence
through the adapter, then only queues work. Repeated queued COMMIT acknowledges
the same ticket without executing callbacks again.

The outer boot loop must observe the queued latch before network, GUI, module
supervision or native dispatch. `check_outer_boundary` must prove that the entire
previous native step returned. A service-stack check and zero active entry slots
alone are insufficient: native syscall dispatch also runs there while its
caller references remain live. A noncopyable `NativeShutdownBoundary` can only
be minted in the actual desktop loop. The loop checks before polling and again
immediately after the entire native step returns, before input or GUI work.

Advancement checks the boundary, revalidates capabilities, closes admission,
stops producers, retires native owners, drains scheduler/VFS resources and seals
storage, in that order. Each callback must be bounded and consume its owners at
most once. Any advancement failure records its exact phase and first status,
holds ordinary work and permanently suppresses power. No later request retries
cleanup. Action consumption rechecks the outer boundary and succeeds once.
`ready_action()` is diagnostic readiness, not evidence of platform execution.
No coordinator operation calls debug-exit, halt, reboot or poweroff.

The legacy `Kernel::request_power_action` now rejects invalid enum values and
returns unsupported for every valid action. The desktop loop has no enum-to-
platform consumer. Diagnostic boot/test/runtime-failure exits remain separate.
The typed internal handoff consumes the coordinator action once. The currently
supported x86_64 halt disables interrupts and permanently holds with `cli; hlt`.
Other nine profiles explicitly refuse admission; reboot and poweroff remain
unsupported. Debug-exit is never a clean platform action.

## Mount admission

`VirtualFileSystem::preflight_owned_mounts` compares the complete attached mount
inventory against exact issued handles, paths, provider/RAM kinds and covered
parent handles. It rejects active namespace operations, unknown or borrowed
attachments, duplicate/stale/foreign handles and ownership mismatches. It calls
no provider, acquires no retained path and consumes nothing. Detached entries
are history and are excluded from the active inventory.

This snapshot is not an admission barrier or drained-owner certificate. The OS
must bind its original `JournalRoot` and original root and `/run` handles; Kernel
must stop every producer before a final snapshot and resource retirement. The
one-way seal sequence remains `/run` detach, root detach and the original
provider's `freeze_clean`. A detach failure may already have consumed references
and must remain sticky. Persistent sync alone does not clear recovery state.
The block transport stays usable until the seal's final checked FLUSH, then
closes request admission and performs a checked one-shot reset. Static DMA
storage remains retained even after a failed reset. Entropy is revoked and its
original singleton reset during producer retirement. Active external network
owners and undeclared GUI/module/driver owners refuse before quiescence. The
initial product admission also requires application windows to be closed; their
modeled user process records do not establish a native service quiescence proof.

`shutdown_reference_status` separately checks all RAM reference counts against
only VFS-owned mount holds, including detached history, and refuses namespace
waiters/socket paths or a historical failed detach cleanup. The original provider
checks its own external references during `freeze_clean`; a mount inventory
snapshot alone grants neither proof.

## Validation scope

The hosted coordinator model includes identity/credential refusals, foreign and
stale tickets, reconstruction at the same address, unsupported preflight,
commit enqueue/idempotence, callback reentrancy, exact ordering and every failure
phase. Its successful fake callbacks certify only mechanism behavior. The
reachable Debug refusal point tests the actual legacy Kernel request without
retiring the test kernel. The exact product desktop loop is also executed with
hosted stubs and the old loop is retained as a failing negative control.

All ten architecture selections in hosted VFS tests remain hosted selections.
Freestanding Debug/Release compilation and standalone QEMU debug boot are
separate evidence lanes. None proves native process retirement, a clean guest
disk, synchronized halt, reboot, poweroff, physical execution or SMP behavior.

## Native ownership and admission

The runtime binds the original init only after successful startup, with a
nonrepeating boot lifetime identity. The caller is resolved from the canonical
running PCB/TCB; native personality, original leader, one live thread and all six
root IDs are checked again. COMMIT queues only after child quiescence. The
adapter rechecks that quiescence at the outer boundary before closing admission.

Native retirement runs the real `finish` path for the original init; it does not
substitute record deletion. Its report covers backend address-space tables,
entry stack leases/quarantine, VM/mapping pages, shared-file references, object
pool pages, syscall origin borrows, VFS continuations, waits and signal owners.
Backend orphan owners and previously contained cleanup errors refuse admission.
Shared/page ownership is verified again after scheduler resource drainage.

Kernel service entries and module/driver/scheduler admission are closed once.
Cleanup can release owners but cannot restart supervision or issue new scheduling,
process/thread creation or descriptor acquisition. This contract assumes current
serialized boot-CPU execution; topology records are not AP execution evidence.
Any later hardware SMP producer needs an independently qualified stop protocol.
