# JBD2 replay execution foundation

`include/ok/fs/jbd2_replay.hpp` executes the actions produced by the existing
checksum-v3 JBD2 recovery planner. It performs real `driver::BlockDevice` read,
write and flush calls. The accompanying tests inject a hosted block device;
they establish executable recovery logic and exact modeled device bytes, not
hardware crash recovery, a writable ext4 implementation or mount admission.

The existing [read-only codec and plan contract](jbd2-model.md) is unchanged.
No ext4 mount, filesystem writer, kernel application, xmake configuration or
OS SDK consumer is enabled or modified by this foundation.

## Device mapping and external ownership

`RecoveryIo::open(device, RecoveryLayout)` admits an explicit **contiguous
internal journal** in one filesystem partition. `first_sector` is the partition
offset in device sectors. `filesystem_blocks`, `journal_first_block` and
`journal_blocks` describe filesystem-block coordinates; `block_size` is 1024,
2048 or 4096 bytes. The device sector size must be a nonzero power of two that
divides the filesystem block size. The entire filesystem and journal must fit
the reported device geometry. All bounds use subtraction/division before address
multiplication or addition. Fragmented inode mappings and external journals are
not admitted through this interface.

Journal reads translate an explicit journal index into device sectors. Home
writes translate a filesystem block index and reject any overlap with the
**entire** journal allocation, including its superblock and inactive ring
blocks. Every transfer is exactly one filesystem block. Each I/O checks that
device size, sector size and writable status still match the admitted geometry.
The default `BlockDevice::flush()` returns unsupported, so drivers without a
real flush implementation fail before recovery writes.

The adapter does not discover or prove ext4 inode mappings. The caller must
derive the journal extent, filesystem length, expected UUID and explicit
permitted metadata ranges from validated filesystem state. The full metadata
domain is checked against the device mapping and journal exclusion before any
I/O. A broad range supplied by an unvalidated caller is not filesystem safety
evidence.

The owner must exclusively quiesce filesystem users, page/block caches and
other device writers, and retain the device and immutable journal identity
throughout recovery. There is no internal lock, cache invalidation protocol or
snapshot-generation service in this foundation. Geometry checks and repeated
checksums do not establish exclusive ownership. Successful device operations
must transfer every requested byte; successful flush must make all preceding
completed writes persistent before returning. Device drivers must satisfy
those existing service contracts.

## Owned plan and execution order

`ReplayExecutor<ActionCapacity, RevokeCapacity, DomainCapacity>` owns two 4096-byte
block buffers, action/revoke arrays and a copy of the metadata ranges. It performs
no heap allocation. Capacities are explicit template parameters with defaults
64/64/32; the integration owner must provide the object in suitable fallible
kernel-owned storage rather than assuming it fits an arbitrary kernel stack.
The executor is neither copyable nor movable, exports no action span, accepts
no externally assembled `ReplayPlan` and admits one serialized run attempt.

`run(io, expected_uuid, domain)` follows these steps:

1. Validate and copy the metadata domain and UUID. Reject ranges crossing the
   journal or filesystem end before any device I/O.
2. Require a successful initial device flush. This checks actual flush support
   before writes; external quiescing must already exclude unrelated pending I/O.
3. Read and validate the actual journal superblock through the bound adapter.
   Its UUID, journal capacity and block size must match the explicit inputs.
4. Build the complete existing replay plan in private storage. Descriptor,
   payload, revoke and commit failures publish no executable plan. Only committed
   surviving actions remain; incomplete tail data and revokes are discarded.
5. Preflight every action's source/destination bounds, sequence/order and payload
   checksum/escape state. An error here issues no destination writes.
6. Read and checksum each payload again immediately before submitting it. Restore
   escaped JBD2 magic only in the private output buffer, after checking its stored
   checksum. Submit full-block writes in exact descriptor/tag order, including
   repeated destinations within and across transactions.
7. Flush all submitted home writes. Only successful final flush publishes
   `ReplayPhase::complete` and `ReplayReport::replay_durable() == true`.

The executor preserves logical submission order; it does not issue a flush after
each transaction or assume individual writes are atomic. The retained journal
is the source of truth for a fresh full replay after interruption. A final flush
also occurs for an empty or entirely revoked action list. This is a statement
about completed replay actions, not a clean-filesystem claim.

## Failure and retry semantics

`run` stops on the first failed read, checksum, bounds check, write or flush.
`ReplayReport` retains the failing phase, returned status, committed/planned/
revoked counts, write attempts/completions and flush attempts/completions.
`destination_may_have_changed()` becomes true before the first write call, since
a failed device write may change none, some or all of its bytes. Completed
write calls alone never imply durability. Any final flush failure leaves
`replay_durable()` false, including when the device persisted some bytes before
reporting the failure.

There is no rollback attempt or automatic retry. The executor cannot be reused
after either failure or success; a second call performs no I/O and preserves the
first report. A caller must resolve the device error, retain/revalidate the same
dirty journal and construct a fresh executor to restart from the beginning.
Full-block replacement with original action ordering makes that restart
idempotent under the documented immutable-journal/exclusive-device contract.
The tests verify this property even when a failed write or flush persisted a
partial block. They do not prove power-loss behavior of a physical controller.

The executor **never writes the journal**, advances its tail, clears `s_start`,
increments the on-disk sequence, clears ext4 recovery state or publishes a root.
Crash-safe journal checkpoint/clean-state updates, orphan cleanup, filesystem
cache invalidation, recovery-on-read-only policy and Linux/e2fsck interoperability
remain separate integration gates. Native crash/reboot tests are required before
using this helper to admit a mounted filesystem.

## Validation and reproduction

Run from the Kernel repository:

```sh
python3 scripts/test_jbd2.py
python3 scripts/test_jbd2_replay.py
ASAN_OPTIONS=detect_leaks=0 python3 scripts/test_jbd2_replay.py --sanitize
python3 scripts/test_jbd2_replay.py --cross
```

The runner first checks the existing fixture against e2fsprogs'
`ext2fs_crc32c_le` oracle. It then uses a sparse hosted `BlockDevice` with separate
volatile and durable sector stores, exact operation tracing and injected faults.
Expected device bytes are constructed independently from the golden payloads,
including escape restoration; full sparse disk maps are compared after success.
Journal bytes and unrelated destination bytes remain unchanged. Tests extend and
independently reseal the fixture to exercise full nonempty replay at all three
block sizes, including bytes beyond the first 1024.

Coverage includes every read-call failure in the golden execution, checksum
changes during preflight and replay reads, every write failure with zero/partial/
full transferred bytes, unavailable/failed initial flush, partial failed final
flush, geometry changes, all-revoked/empty logs, incomplete tails, repeated targets,
ring/sequence wrap, capacity exhaustion, wrong UUID, metadata/journal overlap,
overflowing layouts and 64-bit home targets. Simulated power cuts discard volatile
writes, then a fresh owner replays and must match the exact full data oracle.

ASan/UBSan run the same hosted cases. The shown command disables LeakSanitizer
because this sandbox uses ptrace; address and undefined-behavior checks remain
enabled. The optional cross check instantiates the actual adapter and executor
with all ten explicit freestanding C++23 compilers and verifies each output
object's ELF class, byte order and machine. It only writes temporary objects,
does not configure xmake and does not execute a target CPU. All ten Kernel and OS userland profiles remain represented.
