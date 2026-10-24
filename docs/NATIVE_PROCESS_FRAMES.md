# Bounded native process frame ownership

The seven established QEMU firmware profiles (i386, x86_64, AArch64, RV32, RV64,
PowerPC, ARM32) prepare owned firmware memory before kernel entry diagnostics can
change translations. The complete final linked image, including the OS archive,
extension, bootstrap tables/stack and NOLOAD sections, is reserved with the
physical linker bounds. The existing QEMU recipes load no separate module or
initrd objects. New recipes must provide those reservations before admission.
Discovery or qualification failure stops startup on these seven profiles.

LoongArch64, MIPS and MIPS64 still have no admitted firmware protocol.
Only those three profiles retain the explicit linker-reserved fallback pool.
Hosted tests use separately labeled host backing. No failed supported firmware
profile may select fallback. Native allocation activates only after execution
backend initialization has established the persistent supervisor mappings.

The limit remains 1024 frames globally and 128 pages per process. A canonical
internal registry retains every FrameAllocator owner/generation handle. Hardware
PageMapping and scheduler resource ABIs remain unchanged. Fork allocates distinct
handles; image replacement, anonymous VM replacement, failure rollback and exit
return handles only after retiring the corresponding tables.

If retirement of an abandoned table fails, a bounded internal record retains the
address-space token and its mappings. Each referenced frame receives a pin,
including backing shared with the still-current image. Release requests become
deferred while pins exist. Successful later retirement removes the exact
root/slot/generation record, drops pins and completes deferred releases. Fatal
retirement failures stop the runtime; they cannot make pinned backing reusable.
This is conservative bounded quarantine, not a production asynchronous reaper.

Serial provenance reports preparation source and image/managed bounds, activation
and reclamation counters, and retained physical bounds of each Linux image.
The OS runner can require this provenance together with actual musl heap,
fork/isolation/shared-offset, self-exec, wait and capacity/reclamation probes.
Hosted allocator and fault-injected VM models remain distinct from that native
execution evidence. This change does not provide SMP, arbitrary physical-machine
firmware, DMA ownership, quotas, demand paging, or a general production allocator.

ARM32 uses the explicit SDK 13 `arm32-linux-image` contract at `0x40010000`.
Boot assembly preserves r0-r3 and CPSR/SCTLR/MPIDR. Boot CPU 0 must enter in ARM
SVC with MMU/data cache off before any FDT pointer access. The real r2 FDT is
bounded to the 512 MiB entry access window and its full source storage is
reserved. RAM comes only from its decoded records. Runtime eligibility narrows
to the 256 MiB supervisor aperture; high firmware RAM remains excluded. ARM
serial acceptance requires the entry-state and full FDT source receipts, rejects
source overlap, and checks the entire linked image including NOLOAD sections.
The diagnostic ELF is not an admitted launch image; malformed/missing handoff
stops startup without linker fallback. Historical `arm32.txt` probe fixtures
retain the old direct-ELF negative; `arm32-linux-image.txt` records the new recipe.
