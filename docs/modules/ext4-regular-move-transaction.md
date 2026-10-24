# Cross-parent regular move journal and writer source prototype

This successor adds a distinct journal constructor and a one-shot transaction
writer to the previously selected pure move planner and reversible witness.
All parent source files are retained byte for byte. It does not select a legacy
`directory_rename`, `namespace_regular`, create, allocation or structural-only
recovery fallback.

The scope remains an absent destination in another directory, a singly linked
ordinary regular file, two checked 1 KiB linear directory blocks, and the bounded
32 MiB/four-group profile. No allocation, freeing, replacement, directory move,
external extent tree, xattr update, file-data write or orphan creation occurs.
The original file number, generation, English bytes, extent mappings, ownership,
mode, size, links, atime and file mtime remain unchanged. Both parent timestamps
and versions, and the file ctime and version, follow the pure planner.

`Ext4RegularMoveJournalPlanner::plan` calls the canonical pure planner and encodes
only `OKMVW001`. Its descriptor has exact v3 tags and CRCs, escaped payloads,
commit timestamp/CRC, a zero end block and an active head. It checks descriptor
witness inversion against every complete old home, and structurally parses its
own generated records. These are construction checks, not strict replay proof.
Scratch/output alias checks run before either reset and include borrowed names.
Journal features 16 and 17 are accepted, the old head is empty at sequence at
most `0xfffffffc`, and the new head advances by one without wrap or ring reuse.

`Ext4RegularMoveTransaction` is noncopyable and nonmovable. Its source-prototype
API is public for normal hosted models, and assumes an externally exclusive,
quiesced `driver::BlockDevice`; this does not acquire session authority or a
lock. `prepare(device, bounds, update)` performs only reads. It independently
inspects the device binding, checks an empty orphan chain, acquires both complete
directories, the raw selected inode-table slots and all ancillary preimages,
and constructs the canonical journal. The same raw table bytes are decoded by
the planner and mutated, so there is no separately decoded inode object to trust.
Shared tables are read once, copied consistently and coalesced once, preserving
all selected slot updates. The internal journal inode table is also captured
and its checked mapping bound to the original device binding.

Before even the initial flush, `publish_prepared()` compares the complete current
journal head, superblock, descriptors, internal journal inode table, all eight
allocation bitmap blocks and every canonical home against the original bytes.
It then flushes; writes descriptor, payloads and end; flushes payloads; writes
and flushes commit; writes and flushes the active head. All I/O checks stable
512-byte geometry and admitted partition bounds. A reported failure quarantines
this one-shot writer and preserves attempted/completed counters and failed phase.
A flush that persisted before reporting failure still remains an unconfirmed
publication in the writer report.

Follow-on owned integration can call `logical_images(blocks, images, count)` in
preflight, `original_preimage(block)`, `publish_prepared()` and `report()`. The
logical images are the new head and the unescaped canonical after homes.
`binding()`, `journal_plan()` and `canonical_plan()` expose const construction
inputs for a separate move-specific recovery validator. `matches_published()`
checks exact generated journal record equality after durable publication; it is
not recovery authority. The writer has no checkpoint API. A future owner must
perform strict move replay validation, seal cache participants and bind VFS
identity/budgets before separately checkpointing homes and advancing the empty
head. Mounted classifier, replay policy, owner, syscall and Files integration
remain explicit downstream work.

The authored model uses synthetic checksummed bytes and a memory-only block
model, with a nonzero partition start. It covers five/four/three homes, merged
inode slots, both journal feature profiles, empty files, owned names, alias
refusals, exact preimage changes in every home and ancillary block, invalid
lookup/destination/generation/link/checksum inputs, read and geometry failures,
write/flush ordering, every publication write and flush cut before and after an
operation, and partial cuts of every journal write. It checks that all homes and
file data remain unchanged and failed writers cannot retry. This lane does not
compile or execute that model, create a filesystem fixture, run fsck, export an
SDK or run a guest.

This source prototype does not prove global block ownership, hardware behavior,
strict recovery, mounted atomicity, isolation, native Files readiness or M0–M12
completion. It preserves ten Kernel architecture profiles, twenty mode rows and
four supplied musl profiles without promoting a default SDK.
