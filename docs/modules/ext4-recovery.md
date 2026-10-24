# Runtime ext4 journal binding

`Ext4RecoveryBinding` reads an explicitly owned device extent and derives the
internal journal mapping, filesystem UUID and structural metadata domain. It
performs no writes, flushes, allocation, replay or mount publication. The existing
`Ext4FileSystem::mount` feature gate is unchanged.

The caller supplies a `BlockDevice` and `Ext4RecoveryBounds` in 512-byte device
sectors. Subtraction-form range checks preserve a full u64 partition origin.
Device geometry is checked before and after every read and again before facts
become visible. The object permits one inspection attempt, including failures,
and cannot be copied or moved. `info()` remains null unless the entire inspection
succeeds. Its pointer and the metadata span depend on the binding object's lifetime.

The owner must exclude other device writers, filesystem caches and replacement
for the entire lifetime of any derived facts. A replacement with unchanged
geometry is not detectable here. This synchronous admission object provides no
SMP exclusion or cache-coherence mechanism.

## Admitted format

- Linux dynamic-revision ext4, a primary superblock at the normal offset, 1/2/4
  KiB filesystem blocks and 32-bit filesystem block addresses.
- Exactly `has_journal`; exactly `filetype | extents`, optionally with
  `needs_recovery`; exactly `metadata_csum`. Sparse-super variants, resize/flex
  groups, 64-bit descriptors, alternate checksum seeds, external journals,
  orphan chains/files and other features are rejected.
- At most 32 groups, 32-byte descriptors and 256-byte inodes with the admitted
  journal inode extra-size. Capacity failures reject rather than truncate.
- CRC32C verification for the primary superblock, every group descriptor,
  initialized block/inode bitmaps, internal journal inode and JBD2 superblock.
  Group descriptors must place their superblock, descriptor block, bitmaps and
  inode table in disjoint ranges within each group.
- Canonical `BLOCK_UNINIT` and `INODE_UNINIT` groups are admitted only when their
  checksummed descriptors have consistent empty-group counts and zero unused
  bitmap checksum fields. Those physical bitmap bytes have no authority and
  are not read. Journal blocks/inode cannot belong to an implicit-free domain.
- Internal journal inode 8 must be allocated and identify one initialized,
  depth-zero, contiguous extent. Its size and sector count must agree exactly
  with the extent. Every journal block must be allocated and outside structural
  metadata. ACL/high-address/unsupported journal inode representations reject.
- The existing synchronous JBD2 checksum-v3 codec validates the journal UUID,
  format and error state. JBD2 block size and length must match the inode-derived
  mapping. A filesystem without `needs_recovery` may not have a nonempty log.
  `needs_recovery` plus a valid empty log remains inspectable for a future
  restartable completion path.

The owned output contains at most 128 metadata ranges. A consumer instantiating
`ReplayExecutor` must choose a sufficient `DomainCapacity`; its default of 32
ranges does not cover every 32-group binding. Binding capacity and replay action
capacity are independent and each failure remains explicit.

## Remaining recovery boundaries

These facts are not a full allocation fsck or a mount/replay capability. The
structural domain excludes directory data and external extent-tree blocks; it
does not prove that user-file extents never alias the journal. JBD2 payload CRCs
alone do not establish ext4 semantic validity. A later recovery owner must
validate the final replay view, including unchanged/admitted geometry and journal
ownership, before widening runtime access, clearing recovery state or publishing
a VFS root. Journal-tail advancement, orphan cleanup and transaction writers
remain separate mechanisms.

## Validation

Run the portable hosted suite with an explicit new output directory:

```sh
python3 scripts/test_ext4_recovery.py --sanitize --cross --output /tmp/ext4-binding-run
```

It creates private e2fsprogs images with 1/2/4 KiB blocks and a full 32-group
fixture. `debugfs journal_open -c -v 3` enables the upstream journal checksum
format; `e2fsck -fn` and independent debugfs extent reports check the fixture.
The suite checks validly rechecksummed malformed metadata, each failing read,
changing geometry, u64 origins, one-shot publication, implicit bitmap contents
and absence of writes/flushes. `--linux-dirty-image` additionally inspects a
retained Linux-created capture without modifying it. `--cross` compiles both
the implementation and its API consumer for all ten profiles and checks each ELF
machine/class/endianness; this does not execute those objects.

The ordinary debug Kernel suite reaches an admission-rejection model through
`run_ext4_test`, preserving its previous tests. OS native qualification must use
newly exported architecture/mode-specific SDKs and retains separate actual-device
mapping/replay/reopen evidence; hosted checks cannot establish that execution.
