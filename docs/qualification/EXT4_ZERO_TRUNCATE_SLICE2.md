# Exact linked-inode zero-truncate witness

This source slice adds an off-stack reversible image codec and exact whole-home
classification to the bounded planner. It does not enable mounted truncation,
load code, publish a journal transaction or admit recovery I/O. SDK53 export,
native architecture validation, the empty-reallocation companion and mounted
startup integration remain open.

The private descriptor extension is `OKZTW001`, edition 1. All integers inside
the extension are little endian; JBD2 headers, tags and the descriptor checksum
remain big endian. The extension starts at descriptor byte 128 and has an exact
end offset of 560, consuming 432 bytes. The checked CRC covers the whole 1KiB
descriptor with its checksum bytes zeroed. Logical after images additionally
bind to JBD2 payload checksums, including escape handling.

| Byte offset | Width | Field |
| --- | --- | --- |
| 128 | 8 | `OKZTW001` discriminator |
| 136 | 2 | Edition 1 |
| 138 | 2 | Exact end offset 560 |
| 140 | 1 | Exactly four homes |
| 141 | 1 | Released block count, 1 to 16 |
| 142 | 2 | Reserved zero |
| 144 | 16 | Filesystem UUID |
| 160 | 4 | Nonzero descriptor sequence |
| 164 | 4 | Allocated ordinary inode number |
| 168 | 4 | Preserved generation |
| 172 | 4 | Exact old byte size |
| 176 | 4 | Released allocation group |
| 180 | 4 | Maintained mtime/ctime seconds |
| 184 | 4 | Maintained mtime/ctime nanoseconds |
| 188 | 16 | Four ordered home targets |
| 204 | 16 | Whole before-home CRC32C values |
| 220 | 16 | Whole logical after-home CRC32C values |
| 236 | 64 | Sixteen physical mapping entries |
| 300 | 4 | Reserved zero |
| 304 | 256 | Exact original inode record |
| 560 | 460 | Trailing zero padding |
| 1020 | 4 | JBD2 descriptor checksum |

Unused mapping entries are zero. First-tag UUID bytes and tag padding use the
existing admitted private journal profile. Missing/unknown discriminator or
edition, noncanonical lengths, reserved/trailing data, target/sequence/UUID
mismatch, unknown tag flags and malformed checksums fail closed.

Decoding replaces only the target inode record in the logical after table with
the exact stored old record. It checks the original dense initialized mapping,
requires every released bit to be clear in the final bitmap, restores exactly
those bits, subtracts the release count from final group/superblock counters,
and recomputes the old bitmap, descriptor and superblock checksums. The unchanged
inode bitmap comes from the caller's exclusive checked view. Each reconstructed
home must match its whole before checksum. The pure planner then regenerates
the entire exact four-home transform; every identity field, mapping entry and
byte of before/after images must agree. Retained generation, ownership,
atime/birthtime, allocation and sibling inode bytes therefore remain exact.

The codec does not substitute CRC values for external device/session provenance
or a complete allocation-ownership scan. Before a recovery owner can dispatch,
it must validate the entire committed JBD2 transaction, check both old/final
allocation ownership and bind the exact mount lifetime. Those typed preview
and recovery interfaces are a later source slice.

`Ext4ZeroTruncateHomeMatcher` classifies every actual home as its exact before
or after block and returns a four-bit after mask. All sixteen whole-home mixtures,
including all-after with an active journal, are image classifications only.
Third bytes fail. The matcher does not authorize write completion, repair torn
sectors or recognize an arbitrary copied plan as private recovery authority.

Hosted model coverage includes 1 to 16 blocks, 1 to 4 extent records, partial/full
last blocks, distinct inode/data groups, all inode-table slots, escaped logical
payloads, metadata/identity corruption and overlap refusals. Existing planner,
ownership, first/next-data regressions and the parent/current hosted ABI probe
remain separate. Host compiler frame measurements do not qualify ten native
profiles or complete native call-chain stack use.
