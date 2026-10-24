# Bounded regular-move witness

This private codec preserves reversible image changes for the separate
cross-parent ordinary-file planner. It does not write a journal or device,
classify cold recovery, hold a device session, publish mounted nodes, export an
SDK, or qualify the Files workflow. The existing source remains unchanged.

The descriptor extension uses the distinct `OKMVW001` marker at byte 128.
Three through five distinct canonical home targets, both parent identities,
the file identity, all three generations, event time and both names are stored
explicitly. Full old-block CRCs and canonical maximal runs of changed old bytes
reconstruct the complete before images from the checked after images. The
700-byte patch window refuses overflow before modifying the descriptor.
Unused header fields, tag padding, name padding and trailing bytes are zero.
Encode copies names and preserves the descriptor on every error. Decode names
borrow immutable descriptor storage. Oversized spans and overlapping input or
output storage are refused before reset; subsequent failures clear the witness.
Partly decoded blocks carry no I/O authority.

The codec preserves the existing tag region and outer CRC field. A journal
writer must construct exact tags and recompute the outer JBD2 CRC after encoding.
A cold verifier must validate that CRC and payload CRCs, recompute the complete
planner from the reconstructed before view, compare every target and after image,
and compare a canonical re-encoding. A checksum is not device provenance or a
substitute for those checks. Marker absence or refusal must not select another
recovery family. These transaction, recovery and mounted gates remain required.

The new hosted record model covers three home counts, full preimage reconstruction,
unchanged-byte corruption, metadata and name bounds, reserved bytes, malformed
patches, duplicate targets, capacity refusal, alias refusal without input mutation,
and copied encoder names. Its synthetic records are not filesystem fixtures or
evidence that the planner and cold recovery have been composed. A separate
transaction suite must provide that evidence and all write/FLUSH crash cuts.

All ten Kernel profiles, twenty architecture/mode rows and four supplied musl
profiles remain required. This addition changes neither the SDK version nor any
exported service profile.
