# Bounded journalled first-data allocation

The private SDK47 source adds a first-data path for a fresh empty regular file
created by the bounded SDK46 create operation. Its exported profile is
`ext4_first_data_profile=bounded-mounted-dirty-fresh-empty-first-block-ordered-home-v1`.
The native module interface uses ABI47, while the syscall UAPI remains 0.7.0.
The existing empty-create, rename and ordered-data profile fields remain
separate: none implies this first-block capability.

The path is limited to a 1 KiB ext4 filesystem, a mounted-dirty session with
an empty journal, an allocated canonical empty regular inode, and one write
ending within its first 1 KiB block. Bytes in a gap before the write and beyond
EOF are initialized to zero. The writer selects the first ordinary free block
in the first supported initialized group, checks bitmap and descriptor
checksums and counters, and scans allocated inode records before using the
candidate. It rejects an uninitialized group, a referenced candidate, a
different inode shape, an alias with metadata or journal blocks, and writes
beyond the single-block boundary.

The pure planner changes four metadata homes: block bitmap, group descriptor,
superblock and inode table. The owner seals those images together with the
journal head and the exact initialized data block. It writes and flushes the
data block before journal publication. Recovery validates the proposed
allocation and inode mapping before performing home I/O; completion flushes
allocation homes before writing the inode table, then flushes the inode home
before journal-tail cleanup. Uncertain transport completion quarantines the
mounted session rather than inventing a rollback.

This is a bounded source contract, not a qualification or promotion claim.
The separate hosted, sanitizer, architecture build, QEMU, user ELF and fault
campaign evidence must be recorded before those outcomes can be asserted.
This path does not provide later block allocation, multi-block writes,
replacement rename, unlink, a complete Notes atomic-save flow, physical
power-loss proof, SMP qualification or real-hardware acceptance.
