# Bounded ext4 recovery completion

This mechanism completes recovery for one admitted synchronous CRC32C-v3 JBD2
transaction containing the primary superblock and one ordinary inode-table block.
It preserves the existing mount gate and does not create a journal writer. The
runtime decoder accepts 1 KiB filesystem blocks, the existing internal journal
binding, no orphan/error state, no revoke/incomplete tail, and no cyclic journal
head. Only the checked preview's metadata whitelist is eligible. Empty-log
resumes and already-clean retries are separate states.

## Ownership and preparation

The owner holds one exclusive synchronous device session from before admission
until completion or error retirement. It must exclude mounted users, caches,
other writers and replacement. Geometry checks cannot establish that ownership.
The native probe establishes this precondition concretely: one boot CPU, one
fresh private Virtio disk, no filesystem mounts/caches, and a scoped owner that
allows one active recovery session. It does not claim a general hot-unplug,
SMP-cache-quiescing or storage-lease subsystem.

`Ext4RecoveryCompletion` is noncopyable, nonmovable and one-shot. It privately owns
the checked preview and completion buffers; callers cannot forge an executable
plan. The entire final preview and every retained action are validated before
any write or flush. All allocated ordinary inodes and selected directory targets
are inspected, including during an empty-log resume. The bounded inspection
budget is 128 ordinary inodes and rejects excess with `no_memory`. Unsupported
representations, invalid dot entries, unallocated selected targets, orphan/error
states and bad checksums reject before mutation. This is not full allocation
fsck: cross-inode duplicate-block accounting and arbitrary directory-graph
reconstruction are not provided.

## Persistent ordering and restart states

| Admitted on-disk state | Operation | Required durability before next state |
| --- | --- | --- |
| Needs recovery, retained complete journal | Initial flush, recheck and write all validated home payloads | Flush all home writes |
| Replayed home blocks, retained journal | Set journal `s_sequence` to recovery-next plus one, `s_start` to zero, update journal checksum | Flush the cleaned journal |
| Empty journal, filesystem still needs recovery | Clear only the primary superblock's needs-recovery bit and update its checksum | Flush the filesystem superblock |
| Empty journal, recovery bit already clear | Validate the home view and issue a flush, with no writes | Successful flush before reporting completion |

The already-clean flush is deliberate: a prior failed final flush may have
completed the superblock write but left it volatile. The mechanism never reports
durable success based solely on a clean-looking read. It preserves journal ring
records, backup superblocks, ordinary payload bytes and unrelated metadata.

For the fixture, the successful trace is `flush, home SB write, inode write,
flush, journal SB write, flush, filesystem SB write, flush`. A new object can
restart every prefix of completed operations and every subset of complete home
blocks that a failed flush persisted. No later clean-state write occurs before
the preceding durability boundary succeeds. Report flags identify home, journal
and filesystem durability separately; attempts are recorded before ambiguous
write/flush calls. Any error requires retiring/fencing the device session before
retry. The mechanism does not roll back.

## Torn-write boundary

The device API guarantees complete bytes only on successful writes and durable
ordering only on successful flushes. Failed writes may modify arbitrary prefixes.
The harness enumerates every byte prefix of every submitted write and distinguishes
successful restart from checksum-based rejection. An invalid torn home inode,
primary superblock or journal superblock is rejected without further writes or
flushes. This slice does not reconstruct torn control blocks from backups or
recovery receipts, and must not be labeled arbitrary torn-media recovery. The
journal remains retained until the home flush; after the journal flush, its home
writes are already durable. Some torn writes are byte-equivalent to a valid old
or new state and therefore restart successfully.

## Pinned format and ordering references

The retained reference manifest pins source bytes for Linux 6.13.4 and e2fsprogs
1.47.0. Linux `fs/jbd2/recovery.c:jbd2_journal_recover` increments the end transaction
sequence once more and flushes replayed home data. `fs/jbd2/journal.c` orders
checkpoint completion before marking the journal empty. Linux
`fs/ext4/super.c:ext4_mark_recovery_complete` flushes the journal before clearing
filesystem recovery/orphan flags. This implementation rejects orphan state.

e2fsprogs 1.47.0 `e2fsck/recovery.c` uses the same extra sequence increment;
`e2fsck/journal.c:e2fsck_journal_release` records that sequence, clears `s_start`
and recalculates the journal checksum before ext4 recovery state is cleared.
The exact fixture therefore moves journal sequence 2 to 4, while the planner's
first uncommitted sequence is 3. The clean header follows e2fsprogs 1.47.0's
noncyclic convention, preserving the zero head field. Linux's newer optional
cyclic-head behavior is outside this profile; the read-only round-trip verifies
that the resulting conventional empty journal remains interoperable.

Hosted volatile/durable-store faults, native QEMU scratch-device restarts, and
physical power loss are separate evidence classes. Neither hosted nor QEMU
success proves physical controller persistence or atomic-sector behavior. The
Linux test image has its own pinned binary hash; its prior capture manifest does
not establish correspondence to the independently pinned Linux source tree.
