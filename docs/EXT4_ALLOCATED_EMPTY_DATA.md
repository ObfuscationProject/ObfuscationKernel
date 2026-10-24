# Private allocated-empty first-block planner

This source-only companion plans a first initialized block for an allocated,
linked empty file after zero truncate. It shares the checked empty-inode profile
and geometry validation with the accepted zero-truncate planner, retains the
exact old home images and increments the full 64-bit inode version. It preserves
mode, credentials, generation, access time, creation time and neighboring inode
records. Only size, blocks, inline extent, modification/change times and version
change in the selected inode. All large plan state uses caller-owned scratch.

The candidate must be the canonical first ordinary free block in the selected
allocation group. Exact inode bitmap/checksum/allocation and block bitmap/free
counter checks precede planning. Unsupported metadata, nonempty files, stale
generation, exhausted versions and overlapping inputs/workspaces are refused.

Neither this plan nor its images authorize I/O. Full ownership exclusion for the
free candidate, a complete old-empty witness, typed publication/checkpoint and
startup recovery, mounted lifetime/cache integration, data-before-metadata
ordering and native SDK/userland acceptance remain required. Existing fresh
create planning and replay are unchanged; their canonical-create witness is not
valid recovery evidence for this generalized empty-inode transformation.
