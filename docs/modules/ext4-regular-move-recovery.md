# Regular move cold recovery prototype

`Ext4RegularMoveRecoveryBinding` and `Ext4RegularMoveCompletion` form a separate
source prototype for one active, committed cross-parent regular-file move. They
consume the distinct `OKMVW001` witness. They do not select a legacy structural,
namespace, rename, create, data-allocation or reclaim recovery fallback.

The caller must continuously hold exclusive ownership of a quiesced device,
exclude filesystem caches and other writers, prevent device replacement, and
retain that session after any error until it is fenced or retired. These public
prototype APIs support ordinary hosted source models. They neither acquire this
ownership nor produce a mounted transaction, capability, cache publication,
permission decision or VFS admission.

## Read-only actual device binding

`Ext4RegularMoveRecoveryBinding::inspect(device, bounds)` permits one attempt and
also accepts read-only media. It performs no write or FLUSH. Its owned generic
`Ext4RecoveryBinding` uses public `inspect` to validate actual geometry, ext4
checksums, allocation, internal-journal inode/extent mapping and journal head.
This generic binding does not parse the move family.

The move binding then reads the actual primary superblock, descriptors, four
inode bitmaps, four block bitmaps, complete internal-journal inode table, active
head, descriptor, all three to five escaped payloads, commit and exact zero end.
It validates the observed journal mapping against the generic binding and opens
the actual head using the explicit internal-zero descriptor UUID policy. A
bounded snapshot reader feeds those owned bytes to `jbd2::build_replay_plan`.
Its target domain is bounded outside the journal; the subsequent strict move
validator must authenticate every canonical target before authority to complete
this specific operation exists.

The binding requires a dirty 32768-block filesystem with 1024-byte blocks, inode
8 as the internal journal, one active transaction beginning at journal block 1,
three to five distinct canonical homes, synchronous CRC32C-v3 records, and an
active sequence no greater than `0xfffffffd`. It requires an explicit witness,
exact zero end, no revokes, no incomplete tail and no second transaction. There
is no active or empty-log profile fallback. An empty head is not admitted here.

`Ext4OrphanChainValidator` independently validates an empty orphan chain from the
actual ancillary images. The strict validator restores escaped payloads,
decodes the reversible witness, reconstructs complete before images, reruns the
canonical planner, and compares complete target/before/after images. Each
current home must equal either its complete before image or complete after image.
Torn, unrelated and checksum-invalid homes refuse admission.

Up to 25 distinct complete blocks are sealed. The observed journal means the
entire bounded active record sequence including head and exact zero boundary;
unobserved inactive ring slots are not claimed as sealed. A physical block used
for multiple roles must yield identical bytes. All snapshots and device geometry
are read again before successful admission. `info`, `validation` and
`canonical_plan` return const views only after admission. Snapshot comparison
relies on the continuing external exclusive session; it is not a lock.

## One-shot completion

`Ext4RegularMoveCompletion::prepare(device, bounds)` requires writable media and
owns the binding. It performs actual read-only validation and reports the number
of missing whole after homes. `execute()` consumes readiness before any further
read, so any error retires this instance. It performs this sequence:

1. Compare every exact observed snapshot and stable geometry again. A mismatch
   causes zero writes and zero FLUSH calls.
2. FLUSH the observed active journal before any home write. Actual reads may
   otherwise observe volatile live bytes, which cannot authorize checkpointing
   homes before their journal is durable.
3. Write only homes still equal to their complete before image, using complete
   canonical after images. Already-after homes are not rewritten.
4. FLUSH all home images, including when every home was already after.
5. Replace only the journal head with its checksummed empty form. Its sequence is
   the active head sequence plus two, and its start block is zero.
6. FLUSH the empty journal head.
7. Physically read back every sealed block. Homes must equal canonical after
   images, the head must equal its exact empty image, and every other observed
   image must remain byte-identical. A journal-inode table sharing a home is
   checked against that one complete after image.

The superblock, descriptors, allocation maps, file data, inactive journal slots,
active descriptor/payload/commit/end blocks and source link count are not written.
The original dirty superblock is retained byte-for-byte. Completion is neither a
clean-unmount claim nor a permission to publish a mount. Reports distinguish
attempted writes/flushes, durable homes, durable head and successful physical
verification. A failing write may already have modified the destination.

Successful complete block operations before interruption leave whole old/new
home prefixes while the active head remains published; a new binding can inspect
those prefixes. Torn home/control blocks refuse recovery. This prototype does
not implement empty-head resume, torn-sector repair or retirement of the caller's
exclusive session after failure.

## Qualification boundary

This capsule adds source and handoff instructions only. Preparing and sealing it
does not compile, link, execute a model, access a fixture, run guest/backend I/O,
load a module, publish SDK artifacts, admit a mount or qualify physical hardware.
The named parent source and retained qualification evidence remain unchanged.
All ten kernel architecture profiles, twenty Debug/Release rows and four supplied
musl profiles remain in the copied source manifest.

The owning acceptance runner must use a fresh copied real image and exercise the
actual public APIs. Important cases are a move retaining a 4096-byte source file,
every whole-home old/new combination, shared and distinct table layouts, exact
snapshot/geometry mutations between preparation and execution, malformed or
missing witnesses, foreign/torn homes, sequence overflow, read-only binding,
one-shot refusal, all-after zero-home-write completion, write/FLUSH failures and
physical-readback mismatches. Tests must verify source bytes and allocation maps
remain exact and the filesystem dirty flag remains set. Compiler dependency
captures, actual commands and model evidence belong to that separate execution
owner; this source capsule must not claim them as new successful execution.
