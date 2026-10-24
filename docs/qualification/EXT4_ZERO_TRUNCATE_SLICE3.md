# Committed zero-truncate journal and ownership preview

This source slice extends the independently reviewed fixed witness with a
read-only complete-journal verifier and one-shot transport preview. It does not
enable mounted zero truncation, export SDK53, authenticate a cold device history,
dispatch writes/FLUSHes, or qualify recovery completion. Private publication,
inode-before-allocation checkpointing, counter reservation, mounted lifetimes,
allocated-empty rewrite and both startup routes remain later slices.

The journal verifier requires one active synchronous CRC32C-v3 transaction with
exactly four ordered metadata actions: inode table, data block bitmap, descriptor
block and superblock. The fixed ring starts at block 1; payloads occupy 2 through
5, commit occupies 6 and an explicit all-zero end block occupies 7. Revoke and
checksum-v3 feature bits use the existing admitted format; the 64-bit journal
feature is excluded from this bounded route. Sequence capacity is checked before
an active sequence can be accepted.

The normal JBD2 parser validates descriptor, payload and commit checksums and
requires a complete transaction without an incomplete tail or revoked action.
Every action must match its witnessed physical home, payload position, sequence
and transaction order. Logical escaped payloads are restored before the fixed
witness regenerates the exact old/after transform. The commit image must equal
the private synchronous profile for the exact witnessed mtime/ctime, including
all unused bytes. The journal superblock retains the normal codec's admitted
unused header fields; the transport preview captures and rechecks every byte.

Each observed participant block must equal its whole exact old or after image.
All sixteen mixtures remain accepted image facts, including all-after with an
active journal. This does not assert that every mixture is reachable through a
correct writer's inode-before-allocation barrier. A future recovery owner must
make its empty inode durable before completing allocation homes even when a
supplied mixture was inconsistent with live writer order.

`Ext4ZeroTruncatePreview` claims a disjoint caller-owned workspace once and
captures the eight complete journal blocks, unchanged target inode bitmap and
four physical homes. It opens strict ordinary checked views on two static
overlays. The old overlay substitutes all four reconstructed before blocks;
the final overlay substitutes all four canonical after blocks. Remaining reads
use the same excluded-writer transport. Both views perform complete admitted
allocated-inode ownership scans, including directories, direct symlinks, reserved
records and journal identity, and check every admitted allocation group's
bitmap/checksum/free-count accounting. The old proof must reproduce the exact
released mapping; the final proof requires the same allocated inode/generation
with an empty mapping.

The preview uses caller-owned scratch for strict view opening and root directory
iteration as well as decoding and the ownership scan. Existing convenience
interfaces remain available, and `Ext4ReadOnlyView` fields/layout remain unchanged.
The hosted opaque-reader instantiation reduces the individual super/group/root
iteration frames from about 4KiB to 112/176/176 bytes. These are individual host
compiler frames, not ten-profile or complete native call-chain qualification.

After both scans, the preview rereads every captured journal block, inode bitmap
and physical home and compares whole bytes. Read failure, changed source, third
home bytes, stale identity, aliasing, capacity overflow or invalid accounting
leaves the preview failed and its permanent workspace claim retained. Read
attempt/completion counts are reported as observations only. A derived concrete
counter budget and canonical Session read dispatch are prerequisites for later
execution integration.

The caller must exclude replacement and all writers throughout the preview and
keep the transport, workspace and preview alive together. No borrowed metadata
object, public result or report is a Session or lifetime capability. This source
slice makes no SMP claim and provides no Node publication or cache/VM coherence.

Hosted tests cover all whole-home mixtures at 1, 3 and 16 blocks, exact committed
records, journal corruption, missing/extra actions, forbidden feature flags,
cross-inode and directory/symlink sharing, reserved/journal anomalies, scan
capacity, source changes during preview, every physical read-failure position
of the exercised complete path, and permanent one-shot claims. Model transport
reads and ownership bookkeeping are distinct from native device I/O, durable
FLUSH behavior and musl application execution.
