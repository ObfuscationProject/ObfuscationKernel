# Qualified QEMU runtime frame bridge

This candidate connects owned firmware memory qualification to the bounded
FrameAllocator and separately executes native userspace stores. It does not
replace the process runtime's linker-reserved pool, enable a physical machine,
or introduce SMP, DMA, reclaim, demand paging or allocation quotas.

The caller first discovers genuine firmware, reserves the complete final linked
image (including Kernel/OS extension/archive/NOLOAD), and supplies any separately
loaded objects. qemu_runtime_memory_plan narrows the entry aperture to mappings
that remain valid after the configured execution backend initializes:

| Profile | Retained supervisor access |
| --- | --- |
| i386, x86_64 | Identity first 1 GiB. i386 entry's wider flat 4 GiB aperture is insufficient. |
| AArch64 | The entry 512 MiB window lies inside the execution backend's 1 GiB supervisor RAM mapping. |
| RV32, RV64 | M-mode remains physical; userspace uses separate Sv32/Sv39 translation. |
| PowerPC | Intersection of captured ePAPR extent and the native 256 MiB supervisor mapping. |
| ARM32, LoongArch64, MIPS, MIPS64 | No admitted firmware map; return failure, publish no frame pool and execute no physical page accesses. |

Normalization retains full source and exclusion provenance. The bridge rejects
page-size, 16,384-page, 64-range and native-width mismatches before initializing
bookkeeping. It never silently truncates 64-bit addresses. This bridge alone
does not establish a mapping authority: native backend initialization must
succeed before kernel access, and caller-selected mappings must be retired before
releasing the retained owner/generation handles.

The standalone probe uses the actual boot paths, firmware decoder, normalizer,
FrameAllocator, execution backend and architecture assembly. It exhausts 1,024
owned frames, creates a three-page user address space from these frames, enters
user mode, executes a store and syscall, reads the changed bytes through the
checked user-copy path, destroys the address space, then releases all frames.
It rejects double/stale release and demonstrates generation renewal. If table
retirement fails the probe quarantines frames. It then runs reversible physical
page touches under the established supervisor mappings. All inputs, compiler
identities, PT_LOAD memsz/linker bounds, commands and serial transcripts are
retained. Unsupported profiles have explicit zero-allocation output.

This proves that the bounded ownership bridge and actual user translations can
coexist in these QEMU launch profiles. It does not prove a production allocator,
all user mapping failure paths, hardware cache/DMA correctness, arbitrary boot
recipes or complete M1. Existing standalone execution and VM rollback tests
remain separate requirements. A future process allocator must retain ownership
across fork/exec/map replacement and all table-retirement paths.
