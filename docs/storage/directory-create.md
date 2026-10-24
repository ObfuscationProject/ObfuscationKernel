# Bounded journalled directory creation

Private native module ABI64 adds
`bounded-journalled-single-block-directory-create-ordered-free-init-v2`. Both the Kernel SDK
producer and the ObfuscationOS artifact consumer require its named contract
header, eight-home capacity and nine-buffer owner declaration. The regular
empty-file planner and five/six-home replay policy remain available.

A directory transaction allocates one ordinary inode admitted by the existing
zero-slot or canonical free-generation format and the first supported free data
block. Both the existing parent and new directory use one
initialized 1 KiB block. The new directory contains canonical `.` and `..`
entries and a metadata checksum tail. The planner sets one initialized extent,
size 1024, sectors 2 and link count 2, increments the parent link count, records
full uid/gid and the admitted timestamp, and updates free-inode/free-block and
used-directory accounting with the affected ext4 checksums.

The strict plan/witness preimage must be entirely zero. The ABI64 owner can
read-only prepare a nonzero, proved-unreferenced free candidate, then initialize
it with one zero write, one successful data flush and exact physical readback
before constructing the strict plan. A clear allocation bit alone is insufficient:
the owner repeats the allocated-inode mapping proof and freezes metadata around
that scan; replay repeats the proof through a reconstructed zero old view.
Names fit the existing 31-byte bound. Uninitialized allocation groups and parent
namespace relocation remain unsupported. See ordered-directory-initialization.md.

Seven or eight homes are ordered as inode table(s), inode bitmap, group
descriptors, superblock, block bitmap, new directory data, then the parent
namespace block. Replay accepts only that exact shape and its `OKDRW001`
complete reversible witness. It re-runs the canonical planner, compares every
home image and identity, re-encodes the descriptor, verifies old and final
parent mappings and new directory mapping, and checks the final destination,
`.` and `..` identities. The table prefix, allocation/data prefix and final
parent namespace have separate completion barriers. A distinct common-opening
family reserves up to nine writes including journal cleanup.

Mounted creation uses public retained nodes; the mounted owner keeps the inode
identity and storage lifetime after creation. The POSIX layer applies umask and
parent setgid ownership before the transaction. Failures quarantine the owner;
cold recovery exposes either the old namespace or the completed directory.

The hosted models cover planning, witnessed shape, public node ownership,
POSIX mkdir, nested regular creation, accounting, checksums and transport cuts.
Hosted model results are separate from native Files execution and hardware
qualification. This source change does not implement rmdir, cross-directory
rename, directory rename or automatic SDK promotion.

The prior ABI63 zero-fixture model receipts remain retained. The new ordered
initialization controls read the original SDK44 image with stale deleted-directory
bytes in free block 535 without modifying or sanitizing that file. Explicit
in-memory ordinary-inode and already-zero variants support the hosted controls.
Root alone executes qualification; source preparation is not a model pass.
The strict planner, witness and replay zero-preimage requirement is preserved.
