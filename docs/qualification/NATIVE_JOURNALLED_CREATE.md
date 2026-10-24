# Bounded journalled empty-file creation

This private SDK46 candidate extends the mounted-dirty 1 KiB writer with empty
regular-file creation through `Node::create_child(name, InitialNodeAttributes)`.
It preserves initial mode, UID and GID in the single allocation transaction.
The compiled C++ owner layouts require SDK ABI46; native syscall wire contracts
remain UAPI 0.7.0. SDK41 remains the accepted baseline until separate admission.

The mount reserves a stable result-node address before granting the writer any
authority. It serializes namespace readers, samples active realtime, selects a
clear ordinary inode bitmap bit in an initialized admitted group, and requires a
zero free inode-table slot. The pure planner validates the old superblock,
descriptor, bitmap, parent inode, directory checksum and participant identities.
It inserts one name into an existing directory block with reversible empty
space, initializes an empty extent inode with one link, and updates parent
ctime/mtime/version, inode counters and all relevant checksums. It does not
consume a data block or change free-block counters.

There are five or six deduplicated home participants: parent table, optional
distinct new table, inode bitmap, descriptor block, superblock and directory
block. The canonical owner reserves seven participant slots including the
journal head. Its private sealed authority binds each slot, generation, image,
session, publication epoch and transaction serial. Extending this capacity does
not admit arbitrary additional metadata operations.

The writer validates a proposed journal through the same create-specific
recovery policy before publication. It durably writes payloads, commit and
journal head in that order. Completion first validates the entire log and final
filesystem view without writes or FLUSH. It checkpoints inode tables and flushes
them before allocation homes, then flushes allocation homes before publishing
the directory home. The final home flush precedes journal-tail cleanup. The
mounted recovery bit remains set until the separate drained clean freeze.

A crash can leave the inode bitmap and its descriptor checksum on opposite
sides of the committed allocation. Public recovery binding inspection retains
strict checksum admission. Only the create preview can inspect one provisional
bitmap/descriptor split on a dirty filesystem with a published nonempty journal;
that fact grants no write capability. Replay must prove the exact bounded
allocation, counters, empty inode, parent update and directory insertion, and
validate admissible already-checkpointed homes before completion performs I/O.
An unrelated or malformed transaction is rejected before any home write.

Benign validation, capacity and ENOSPC failures drain staged pins and preserve
the prior mounted view. Any uncertain transport completion quarantines the
session and mount. No retry allocates from potentially stale bitmap state and no
rollback is manufactured after an uncertain write or FLUSH. The node is published
only after checkpoint, exact disk/cache/image comparison, renewed binding and
metadata checks.

This is serialized boot-CPU behavior. It does not qualify SMP operation, torn
home-block reconstruction, physical power-loss durability, uninitialized inode
group initialization, inode reuse, non-regular creation, special permission
bits, directory growth, general allocation, replacement rename or unlink.
Creating an empty Notes temporary file is one prerequisite; Notes still needs
data allocation, replacement and cleanup to execute its default atomic save
workflow. Hosted models, cross compilation, Kernel QEMU execution and actual
user ELF integration must be reported as separate evidence lanes.
