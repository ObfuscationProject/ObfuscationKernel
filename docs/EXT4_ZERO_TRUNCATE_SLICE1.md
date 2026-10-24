# Zero-truncate source slice 1

This source slice implements pure linked-inode zero-truncate planning and
read-only ownership proofs. It is a candidate for SDK53, with ABI51 unchanged
in the measured 64-bit hosted layout. No SDK53 artifact has been exported and
the mounted provider still rejects actual zero truncation. This slice does
not enable production truncation or close M4/S4/M0-M12.

`Ext4ZeroTruncatePlanner::plan_off_stack` admits an allocated ordinary one-link
regular inode with an exact initialized inline mapping of zero to sixteen
1KiB blocks in one initialized data group. It reuses the existing checksummed
geometry and initialized-inode profile rather than widening feature admission.
It validates inode allocation/generation/size, bitmap checksums and free counts,
reserved and padding bits, mapping uniqueness, maintained timestamps and version
capacity before producing any home image. Scratch/output and input overlap is
rejected before modifying workspace.

Nonempty plans carry four exact before/after homes in inode-table, data-bitmap,
descriptor and superblock order. Only the proven data bits and block counters
are released. The inode remains allocated and retains its generation, owner,
mode, one link, access/birth times and unaffected bytes; its size/sectors/mapping
become empty, modification/change times advance and its 64-bit version increases.
Inode bitmap, free-inode accounting and sibling inode bytes are preserved.
Empty-to-empty returns a validated no-op with zero homes, even at maximum version;
it does not consume a new timestamp or increment version.

`Ext4ZeroTruncateOwnershipValidator::prove` scans all admitted allocated inode
records and independently checks allocation bitmap/checksum/free-count accounting
in every group. It rejects overlapping ownership, reserved/journal references,
changed inode records, a release crossing groups and exhausted scan capacities.
It rechecks the target after the complete scan. The view remains exclusive and
immutable by contract; reader failures quarantine it. Public results are facts,
not write, lifetime or reclamation authority.

New calls take caller-owned bounded workspace. The global ownership proof and
private decoder/allocation helpers now accept an off-stack block scratch buffer;
their previous convenience interfaces remain present. The optimized opaque-reader
host compile records planner frame 304 bytes, validator 592, global-proof 400,
decoder 160 and allocation 176. These are individual 64-bit host compiler frames,
not a complete native call-chain, IRQ or architecture stack qualification.

Build the separate hosted model project from `tests/host/zero_truncate` with
explicit `-P .`, an isolated `XMAKE_CONFIGDIR` and an external build directory.
The new exact-image suite has 299 cases, the ownership suite 30; existing first-
and next-data image plan suites remain passing. The parent/current layout probe
checks ABI51 and selected module/Owner sizes, alignments and manifest offsets on
the host. Neither model checks nor a freestanding compile-only archive qualify
native device execution, process integration or ten-profile ABI closure.

Remaining required work is the private zero-truncate witness, exact mixed-home
preview/recovery, journal publication and inode-before-allocation checkpoint
ordering, phase/counter budgets, mounted lifetime/cache integration, generalized
allocated-empty reallocation with its exact original-empty witness, and both typed
mounted-startup recovery families. Production enablement must wait for these
companions and independent source review followed by fresh ten-profile native
SDK/OS-consumer and four-musl acceptance. Sector tears, general shrinking, wider
files/groups/extent trees, multi-link/unlinked-retained operations and coherent
file-backed mappings remain separately open.
