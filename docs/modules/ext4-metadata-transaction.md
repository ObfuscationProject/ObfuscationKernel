# Bounded ext4 metadata transaction

This design precedes implementation. It permits one ordinary regular inode's
mode/owner/ctime/version update in the same 1 KiB, synchronous CRC32C-v3 internal
journal format admitted by recovery completion. It provides neither a mount nor
a general filesystem writer. Allocation, data, mappings, links and namespace
remain immutable. The owner holds one exclusive quiesced device session with no
mounts, caches, competing users or replacement until success/error retirement.

## Recovery compatibility

The existing completion profile is sufficient if the journal superblock is
published last. Only clean+empty, dirty+empty and dirty+complete-two-action states
become reachable at completed-operation boundaries. Incomplete transactions are
never published. The existing validation/preparation will be factored privately
for reuse; no recovery admission state or checksum rule is widened.

The input must be clean with an empty journal. A caller first completes any
earlier published transaction using a separate completion owner. The writer is
one-shot and owns its buffers. It validates all current allocated ordinary
inodes under the existing 128-inode budget, constructs the complete proposed
transaction, then validates the synthetic published view through the same
completion preflight before the first real write or flush. Caller-supplied
inode-table or journal offsets are never accepted.

## Layout and persistence order

The journal ring starts at block 1. The fixed private staging area is descriptor
1, dirty-superblock payload 2, updated inode-table payload 3, commit 4 and an all
zero end sentinel 5. Input journals with another first block or fewer than six
blocks are rejected. The sentinel prevents a stale next-sequence record from
being interpreted as a second transaction. The transaction sequence is the
empty journal sequence plus one, matching Linux empty-log startup. Values near
wrap are rejected so both transaction and completion's extra increments remain
nonzero; no circular transaction-number reuse is claimed.

| Step | Device operation | Persistent state after successful flush |
| --- | --- | --- |
| 1 | Preflight flush after all validation | Clean filesystem, empty journal, old inode |
| 2 | Write descriptor, both payloads and zero sentinel; flush | Unpublished staged records, old inode |
| 3 | Write checksummed commit; flush | Unpublished complete transaction, old inode |
| 4 | Set primary filesystem needs_recovery, update checksum; flush | Dirty filesystem, empty journal, old inode |
| 5 | Publish checksummed journal sequence/start; flush | Dirty filesystem, complete recoverable transaction |

Success means durable publication, not home-block checkpoint completion. The
caller uses the existing completion mechanism to replay/checkpoint, clean the
journal and clear recovery. Before publication, recovery preserves the old inode;
after publication it applies the new inode. A durable commit block by itself is
not the publication point while `s_start` remains zero. An ambiguous publication
write/flush may recover either old or new; no rollback or exactly-once receipt is
claimed. Retrying an unpublished attempt may reuse its unpublished sequence only
because all descriptor/payload/end bytes are rewritten and flushed before the
new commit and publication. Published attempts must complete before a new writer.

Failed operations stop immediately. Whole completed-operation prefixes and all
subsets of complete staged blocks persisted by a failed flush are restartable.
Torn staging bytes stay unreachable until a later successful full rewrite.
Invalid torn primary/journal superblocks remain fail-closed under the existing
completion contract; this slice does not reconstruct them. Geometry checks are
additional checks, never proof of exclusive ownership or physical media identity.

## Independent format rationale

Pinned Linux 6.13.4 `fs/jbd2/commit.c` supplies descriptor flags, sequence-seeded
payload CRC32C, descriptor tail CRC32C and zeroed-field commit CRC32C. Its
synchronous commit uses PREFLUSH+FUA after payload completion. Separate successful
payload and commit flushes provide that ordering through this project's API.
`journal.c:jbd2_journal_update_sb_log_tail` publishes sequence/start with a journal
superblock checksum. `recovery.c:jbd2_journal_recover` ignores ring records when
`s_start=0`, and advances the recovered end sequence once more. Linux normally
publishes a nonempty tail before its transaction completes; this bounded writer
delays publication to avoid introducing the currently rejected incomplete-tail
state. Delayed publication is a scope restriction, not a claim to implement
Linux's complete journal engine.

Pinned e2fsprogs 1.47.0 recovery independently verifies the resulting descriptor,
payload and commit bytes and cleanup. Source URLs and hashes are retained in the
batch reference manifest. The Linux binary identity is separately pinned; its
correspondence to the source tree is not established.

Hosted volatile/durable fault models, SDK-only native QEMU device probes,
independent Linux/e2fsck recovery and physical power loss remain separate evidence
classes. No model/native success is presented as physical controller durability.
