# ext4 node cache eviction

Slice of `fix/ext4-node-cache` (Kernel issue #7). In the ObfuscationOS musl
guest every lookup, create and exec on the ext4 root failed with `ENOMEM` after
about 110 new files; only a reboot recovered.

## Causes

- Both ext4 providers keep runtime node identities in a fixed 128-slot array
  and never returned a slot: `Ext4FileSystem` (`src/fs/ext4_fs.cpp`, the
  non-journalled root the guest image mounts) and `Ext4MountedMetadata`
  (`include/ok/fs/ext4/mount.hpp`, the journalled root). A slot was reused only
  after an unlink retired its inode.
- The journalled writer's lifetime table (`Ext4InodeLifetimeTable`, 128
  records) never forgot a linked inode either.
- Independently of the cache, the journalled writer's create and data paths
  stopped at 128 allocated inodes: the ordered-data, first-data and next-data
  ownership scans, the create and directory replay scans and the completion
  inode inspection each returned `ENOMEM` past 128 in-use inodes.

## Change

- A node slot whose identity nothing can reach is evicted when no free slot is
  left: no references or leases, never handed out as a raw `Node *`
  (`borrowed_identity_exposed_`), not the root and, in the journalled writer,
  not the pending orphan or the node under mutation and not held in a lease
  slot. The journalled writer also drops the inode's lifetime record
  (`Ext4InodeLifetimeTable::forget`, refused while any hold exists or the inode
  is detached). Dirty data and in-flight transactions hold I/O leases, so they
  stay resident until they finish. A later lookup observes the inode again from
  the checked image. Neither class layout changes.
- The candidate scans named above are bounded by the filesystem's inodes
  instead of 128; each still proves the candidate against every allocated
  inode. The composed recovery seal no longer requires at most 128 checked
  inodes.
- A borrowed VFS mount (`VirtualFileSystem::mount`) keeps the directory it
  covers as a raw pointer with no reference. Once slots are evicted that
  identity could be recycled for another directory, which then resolved as
  the mount (in the guest, `/dev` captured a new directory after about 120
  files). The covered node now records permanent borrowed exposure, as the
  mount root already did; owned mounts hold a reference and were unaffected.

## Tests

Hosted (`scope=model`):
- `test_ext4_mounted_create.py` (clean image from
  `scripts/make_clean_ext4_fixture.py`): one mount creates 400 files in seven
  directories beside a borrowed mount over an eighth, reopens each, checks the
  mount still covers its directory and no other directory resolves to it, writes a first and a block-aligned next data block
  to one of them, keeps an open descriptor's node resident throughout, keeps
  the lifetime records within capacity, and `e2fsck -fn` finds the result
  clean (421 inodes).
- `test_ext4_fs.py`: the non-journalled writer creates 320 files in eight
  directories on 1, 2 and 4 KiB block images and reopens each; offline
  `e2fsck` stays clean.
- `ext4_next_data_ownership_tests.cpp` now expects the next-data scan to admit
  129 allocated inodes.
- On top of `chore/hosted-runners`: the 124 self-contained and 5 fixture
  runners pass.

QEMU guest (`scope=hardware`, ObfuscationOS paired branch, SysToolchain 0.1.0,
QEMU 10.2.0): the new `sh-acceptance.sh` check `many_files` creates and stats
320 files on the ext4 root in one boot (40 per directory) and removes them; all
36 checks pass on x86_64, arm32 and ppc. Standalone `xmake test` passes on the
same three profiles.

## Remaining bounds

- The journalled writer's global ownership proof, used by zero-truncate and
  first-data replay, keeps its 128-inode bound: the zero-truncate session read
  reservations (`A=1420+531G`) assume it. Recovery admits only its bounded
  32 MiB domain. Both belong with WT2.4 `feat/storage-durable`.
- Directory listings past 64 entries (`max_child_nodes`) are paged since
  `feat/linux-personality` (#6); before it `getdents` failed with `EOVERFLOW`.
- The journalled writer admits a first data block and block-aligned next
  blocks only; an unaligned second write returns 0 bytes. Unchanged here.

## Receipts

Validated at module ABI 77 on `feat/linux-personality` `6968183`, which merges the
four P1 review branches, with ObfuscationOS `feat/linux-personality` (guest runs at
`8cb051a`, tip `9d9bae5`): ObfuscationEvidence `receipts/ObfuscationKernel@6968183/feat/linux-personality/{hosted-runners,os-consumer}` and `receipts/ObfuscationOS@9d9bae5/feat/linux-personality/userland-gate` (`many_files` on all ten profiles).
