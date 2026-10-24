# Exclusive synchronous block sessions

Storage plan section 6 requires explicit ownership and truthful synchronous
completion before a journalled mount. The existing scratch probes supply these
conditions locally; this slice makes their request admission, stale-handle and
error retirement rules a reusable Kernel service. It does not enable a mount or
replace the old sector cache with a transaction cache.

One authoritative `BlockSessions<Capacity>` service owns registration/publication
for its devices. The boot/device owner registers each raw device once, supplies a
stable nonzero identity, retains its storage, and stops publishing raw aliases.
Duplicate device pointers or identities reject, including quarantined entries.
Independent service instances and pre-existing raw aliases are outside the
enforcement boundary; geometry cannot discover them. Future caches and mounts
must obtain this service's views rather than retain an independent raw writer.

The service reserves fixed caller-owned metadata for a bounded number of devices;
registration or session exhaustion is explicit. It does not allocate in I/O paths.
Copyable registration keys carry service identity and generation. Noncopyable,
nonmovable session objects are one-shot and additionally carry a session serial.
The service must outlive every registration key, session and outstanding call.
Keys cannot survive service destruction or reconstruction at the same address.
The raw device/backing storage remains owned throughout registration/quarantine.
Initialize a session completely before publishing it to concurrent callers;
acquisition/destruction is not concurrent object construction or reclamation.

## State and ownership

| Device state | Allowed transition | Guard |
| --- | --- | --- |
| Unregistered | Reserve -> Registering -> Ready | Reserve unique device/identity before unlocked geometry query; invalid geometry releases the slot |
| Ready | Acquire -> Owned | Valid generation, bounded partition, fresh session |
| Owned | Submit synchronous request | Same owner/serial, no request in flight |
| Owned | Close -> Ready | No request in flight, no unflushed writes, no error |
| Owned | Abort/error/dirty abandonment -> Quarantined | Immediate admission stop |
| Ready | Detach -> Unregistered | No active owner or request |
| Quarantined | No reuse or detach | Retain device/backing until external shutdown/fencing |

Every session is exclusive, including readonly sessions. Partitions are bounded
relative device views, but exclusivity applies to their entire registered raw
device; overlapping and disjoint partitions cannot accidentally create separate
writers. Invalid bounds, readonly writes and concurrent submissions reject before
calling the device. Session geometry stays immutable for its lifetime. Geometry
changes before/after a device operation quarantine the registration.

A short spinlock protects metadata only. It is released before geometry queries,
read/write/flush calls and reacquired for completion. A request pin survives that
unlocked interval. Concurrent or reentrant admission/close/detach returns busy;
there is no implicit queue or sleep under a spinlock. This is task-context
synchronous ownership, not IRQ-driven queues or proven hardware SMP execution.

Session attempted/completed READ, WRITE and FLUSH history uses the fixed
`SessionCounter = u64` type on every architecture. Storage capacities, spans,
transfer sizes and indices keep native `usize`. The metadata spinlock protects
all six counters and report snapshots, including on 32-bit targets. Before any
selected dispatch, both its attempted and completed counters must have one
remaining credit; refusal causes no runtime device callback or transport. An
attempt is counted before callbacks and successful completion is credited only
after the original guarded authority remains exact. Pure finite-width policies
use checked `capacity <= limit - used` arithmetic without changing a Session.

This source-only accounting successor changes native C++ report/result and
embedded service layouts on 32-bit profiles. Export requires a separately
coordinated native module ABI revision, matching exporter/OS validator changes,
and rebuilt architecture/mode/compiler SDK artifacts. Existing ABI51 artifacts
must not be mixed with these headers. No export or installation is qualified by
the hosted accounting model.

Each submitted write marks pending durability before the device call. Successful
flush clears that pending state only after all preceding admitted requests have
completed; concurrent new requests reject rather than queue behind a flush.
Successful transfer and successful persistence remain distinct. Close never
performs hidden I/O. It returns busy for pending writes so the owner can explicitly
flush or abandon. Successful close says nothing about filesystem cleanliness,
unmount state or whether a committed journal still needs checkpointing.

Any device I/O error or explicit integrity abort quarantines the registration,
including a failed read, conservatively. Further I/O, acquisition and detach
reject. Dirty abandonment does the same without issuing corrective writes.
There is deliberately no reset/recovery escape in this first service: the old
BlockDevice API does not prove cancellation/fencing after a DMA timeout. Device
and DMA backing remain owned until the transport's enclosing lifetime ends. A
fresh QEMU boot gives the probe a new device lifetime for on-disk recovery.

Readonly and flushed sessions may close or leave scope without mutation. An
active call still requires its caller-owned spans and session/service lifetimes;
this synchronous API is not asynchronous buffer ownership or a defense against
arbitrary C++ lifetime violations. Reentrant explicit abandonment records a
quarantine and retains the request pin until the call returns.

## Integration and validation boundary

Registration reserves its identity/slot under the metadata lock before querying
geometry outside the lock. Concurrent duplicate first admission and duplicates
of owned/in-flight/quarantined devices reject without any raw virtual call.

The native consumer runs the reviewed ext4 writer and recovery through an SDK
session view and checks alias/bounds/readonly/quarantine rejection.
The ext4 format, metadata whitelist and mount rejection remain unchanged. Old
sector-cache tests remain with their original implementation; no unaware generic
writeback cache is inserted beneath the journal.

Hosted tests exercise lifecycle, stale generations, capacity, every transfer and
flush error, reentrant admission/retirement, geometry changes and concurrent
request exclusion with real host threads. All-ten freestanding compilation and
native QEMU device consumption are separate evidence from host concurrency.
Physical DMA fencing, cancellation, IRQ/SMP device operation, shared-storage MMP,
canonical transaction buffers and mount integration remain later prerequisites.
