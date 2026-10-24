# Bounded metadata-buffer ownership

## Scope and baseline

This header-only mechanism builds on the exclusive block-session service without
changing existing driver layouts or vtables. It does not open modern ext4 mounts,
perform home writeback, or provide a production allocator.

Plan grounding: STORAGE_EXT4 sections 3 and 6 require fallible reservations,
canonical identity, transaction pins, generations and private versions before
writeback. A boot/caller-provisioned bounded arena is permitted as an early path;
production reclaimable allocation remains an independent prerequisite.

## Ownership and identity

`MetadataBuffers<Slots, BlockBytes, DeviceCapacity>` owns a private one-shot
BlockSessions session. It accepts the authoritative service registration and a
partition, never an arbitrary raw BlockDevice pointer. Only that private session
loads canonical blocks. No raw session or mutable byte span escapes. Another
buffer owner for the same registered device cannot bind, including another
partition. Independent registries and retained raw aliases remain excluded by
the service publication contract.

This mechanism validates partition bounds, not ext4 metadata classification. Its
caller must resolve admitted metadata addresses using the checked filesystem
decoder; no arbitrary caller block can acquire a writeback capability.

The binding identity contains physical identity, registration generation, session
serial, immutable partition and compile-time block size. Per-slot nonwrapping
generations prevent released pins from referring to reused data. The owner itself
is one-shot and noncopyable/nonmovable; same-address reconstruction with retained
handles is a lifetime violation, not a supported nonce reset.

Caller-provided `Pin` and `Transaction` objects are noncopyable/nonmovable and
one-shot. The owner must outlive all such objects, all outstanding calls and
registry keys. No call races object construction/destruction. The registry and
raw-device backing outlive the owner and its session. Owner open, Pin acquisition and Transaction begin must each finish before the
corresponding object is published with synchronization. Do not race any handle
method with its initial binding; methods inspect their owner pointer before
delegating to the locked owner. Destruction without explicit clean close
quarantines the session. Neither C++ destructors nor bookkeeping prove fencing;
quarantined owner storage, including in-flight read buffers, must remain retained
until the enclosing transport lifetime ends or external cancellation is proved.

## State and private versions

Each slot has one canonical immutable block, a separately reserved private image,
read pins, I/O state, generation and transaction serial. Duplicate hits share the
same canonical version. Misses reserve an unused/clean-unpinned slot before
unlocked synchronous I/O. Only clean unpinned, transaction-free slots are evicted.
Capacity failures return no_memory before I/O and never imply disk no_space.
A pending miss reserves its eventual read-pin credit before unlocking, so cached
hits cannot exhaust its counter while the read completes.
Opening preflights the checked session transfer limit; a filesystem block larger
than that bound rejects before I/O and releases its clean session without
quarantining the healthy device. The accessor rejects unbound, closed/stale and
quarantined sessions and exposes no mutable transport capability.
One device request is admitted at a time; cached reads may proceed concurrently.
No lock is held over a device call.

A transaction begins from a bounded distinct set of live pins of this owner.
Validation of every participant and all credits precedes any private edit. One
transaction owns those slots; read pins may then be dropped without eviction.
Canonical readers continue to see the old immutable version. Updates/readback
copy bytes through checked methods; no mutable reference escapes. Abort/destructor
discards only private versions and releases transaction pins without device I/O.
There is intentionally no caller-asserted commit, publication or checkpoint API;
future integration must consume real journal durability evidence. This is a
reservation/private-version dependency slice, not a complete journal cache.

A device read error or explicit integrity abort fails the owner, quarantines the
session and rejects new operations/reuse. Existing backing is retained. Read pin
and transaction retirement may clear logical ownership but cannot re-enable
admission or release the device. Close requires no pin, transaction or I/O and
is distinct from filesystem clean/checkpointed state.

## Qualification boundary

Hosted ASan/UBSan tests: duplicate hits, exact block bytes, clean eviction,
stale/foreign/closed pins, all-participant transaction preflight, canonical/private
separation, transaction pin pressure, abort/destructor rollback, readonly views,
1/2/4 KiB blocks over supported sector geometry, large offsets, capacity errors
before I/O, geometry/error quarantine, real-thread in-flight exclusion and
reentrant no-lock device callbacks. Preserve existing session tests.

Native SDK-only QEMU probes: read validated ext4 metadata from private disks,
exercise shared pins, private edits and discard, prove disk bytes unchanged,
reject alias ownership, and inject completed-read errors to check no reuse/I/O.
Use all ten architecture profiles and fresh standalone Kernel boots. Hosted
threads do not establish native SMP; injected completed-operation errors do not
establish DMA cancellation or physical power-loss behavior.
