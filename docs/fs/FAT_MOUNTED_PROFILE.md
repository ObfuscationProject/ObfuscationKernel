# Mounted FAT32 and exFAT profile

`fs::FatFileSystem` owns the native, bounded FAT32/exFAT VFS provider.
`mount(BlockDevice &, bool writable = true)` admits the whole volume. The
caller mounts `*root()` at its chosen VFS path and keeps both the provider and
block device alive for every reference. Distribution mount policy belongs to
ObfuscationOS. The provider cannot be copied, moved, or rebound.

## Implemented operations

The provider supports directory listing, case-insensitive ASCII lookup, regular
file reads/writes, zero-filled gaps and extension, truncation, file/directory
creation, file unlink, empty-directory removal and ordinary sync. FAT32 long
names and 8.3 aliases share one namespace; new aliases avoid existing long names
and aliases. exFAT supports checksummed entry sets, allocation bitmap/upcase
validation and reading/extending `NoFatChain` files. Both formats use the actual
mounted VFS interfaces rather than treating boot-sector parsing as a filesystem.

Permissions are fixed at root-owned 0644 files and 0755 directories. Readonly
attributes remove write permission. Creation rejects other modes or nonzero
UID/GID before writing. Rename, hard links, symlinks, chmod, chown, general
Unicode and maintained POSIX inode/timestamp attributes are unsupported. The
attribute-validity mask does not claim unavailable inode/timestamp metadata.

## Bounds and ownership

The admitted profile uses 512-byte sectors, at most 128 sectors per cluster,
262144 data clusters, 1024 clusters per chain and 128 node identities. FAT32 must
also have at least 65525 data clusters. Names must fit the existing 31-character
VFS component limit. The general VFS read/list buffer bounds still apply. These
bounds are explicit admission/operation limits, not full FAT/exFAT coverage.

A complete bounded namespace scan rejects cycles, crosslinked clusters,
invalid allocation data, unsupported records and names before enabling writes.
FAT32 mirrors and clean flags are checked. Both exFAT boot regions, checksums,
allocation bitmap, entry-set/name checksums and ASCII upcase mapping are checked.

Retained lookup/create acquires lifetime ownership within the provider operation.
Unlinked files remain usable through held references and release their data on
the final release. Unexposed, retired node slots are reusable; borrowed raw-node
identities remain tombstones for the provider lifetime. The 128-node bound counts
live nodes, retained unlinked nodes and borrowed tombstones together. Pure VFS
retained create/delete loops can exceed 128 total operations. Operations serialize
through a bounded busy gate; concurrent admission may return `busy`. This is not
SMP filesystem qualification.

## Sync and failure policy

Writes mark the media dirty before changing allocation or namespace state.
Ordinary sync flushes changes, publishes the clean flag, then flushes again.
Sync returns `busy` while an unlinked retained file still owns clusters, so an
orphan allocation cannot be published as clean. Successful final release makes
the next sync eligible to publish clean state. Allocator hints are marked unknown
rather than claiming exact free-space counters.

This provider is nonjournalled. Device write failure or an operation failing after
partial mutation disables further writes. There is no rollback or power-loss
qualification; dirty/failed media must be checked by an external filesystem tool
before another writable mount. `sync_persistent()` remains unsupported. A
successful ordinary sync is not evidence of physical-device durability.

## Validation

`tests/kernel/roadmap/network_storage_tests.cpp` invokes a sparse writable FAT32
block model through the Debug roadmap suite reached from `kernel_main`. It checks
create/read/write/gap/truncate/unlink/sync and a fresh-provider remount. The test
remains a block model even when the containing Debug kernel boots in QEMU.

Run independent hosted interoperability and sanitizer controls with:

```sh
python3 scripts/test_fat_fs.py --sanitize \
  --tools=/explicit/path/to/mkfs-and-fsck-tools \
  --output=/new/output/directory
```

The runner uses xmake, independent `mkfs.fat`, `mkfs.exfat`, `fsck.fat` and
`fsck.exfat`, snapshots source hashes, and preserves command logs. Coverage
includes mounted VFS retained unlink, 512 create/delete reuse cycles per format,
LFN/alias conflicts, remount data, malformed signatures, root cycles, crosslinks,
FAT32 mirror/dirty state, exFAT boot checksum/bitmap, eight write-failure boundaries
per format and a reference-checked contiguous exFAT file. Hosted ASan/UBSan
coverage does not establish guest, physical hardware, concurrent access or
power-loss safety. Guest SDK-consumer acceptance is separate evidence.
