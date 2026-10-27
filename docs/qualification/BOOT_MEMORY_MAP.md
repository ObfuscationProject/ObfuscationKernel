# Discovered boot memory map on ten profiles

Slice of `feat/mm-general` (WT1.1). `kernel_main` now hands `Kernel::boot` a
physical memory map discovered from the loader on every preserved profile, and
the kernel frame allocator manages that RAM with metadata carved from it. The
synthetic 64 MiB map is compiled only into hosted model builds.

## Discovery per profile

`boot::discover_kernel_memory` runs once on the boot CPU, from
`user::native::prepare_memory()` in `kernel_main`, before debug tests or the
execution backend replace entry translations. It reads through the QEMU entry
reader (`QemuProcessMemoryReader<A>`), decodes the loader source, applies the
kernel-lifetime plan and qualifies the result.

| Profile | Protocol | Source | Managed window |
| --- | --- | --- | --- |
| i386, x86_64 | `bios-e820` | BIOS E820 table at `0x5000` | identity first GiB |
| aarch64 | `aarch64-linux-image` | FDT `/memory`, `/reserved-memory`, reservation block | 1 GiB at `0x40000000` |
| arm32 | `arm32-linux-image` | FDT (r2) | 256 MiB at `0x40000000` |
| rv64, rv32 | `riscv-fdt` | FDT (a1) | M-mode physical (Sv39 PPN / 32-bit) |
| loongarch64 | `loongarch64-efi-fdt` | UEFI system table (a2) → `EFI_DTB_TABLE_GUID` → FDT | DMW0 identity, 48-bit |
| mips, mips64 | `malta-prom` | PROM environment `memsize`/`ememsize` sizing the Malta board table | KSEG0 512 MiB |
| ppc | `epapr` | FDT (r3), ePAPR mapping extent (r7) | boot TLB1 256 MiB |

The managed window is RAM the supervisor keeps reaching after the execution
backend initializes and that the backend accepts as user backing. Firmware RAM
outside it stays recorded as an `unmapped` or `address-width` exclusion; it is
never silently dropped and never managed.

LoongArch follows the Linux LoongArch boot convention: a0 = 1 (EFI), a1 = command
line, a2 = system table. The 64-bit system table signature, table count (at most
64) and pointer are validated, and exactly one device-tree configuration entry is
accepted. The system table and configuration table are reserved as loader
storage; the FDT is the source storage. QEMU 8.2 passes no system table for a
direct ELF kernel and its boot therefore stops with `source_absent`; the pinned
QEMU 10.2.0 passes one.

Malta is a board table, not a firmware map. The loader passes argc, argv and
envp as 32-bit KSEG0 pointers (sign-extended on mips64) and the low RAM size in
a3. The decoder reads at most 32 envp pointers and 256-byte strings, requires
`memsize` to equal a3 and `ememsize` to be consistent with it, and rejects a
repeated or unparsable size. Low RAM is `[0, memsize)`; RAM beyond the 256 MiB
I/O hole is the `0x90000000` alias and lies outside KSEG0. The first page
(exception vectors) and `[0x10000000, 0x20000000)` (I/O, PCI, GT-64120, flash)
are board reservations, and the PROM block is source storage.

## Reservations

Reservations always win over RAM declarations; partial pages are excluded.

- Firmware: E820 non-RAM records, FDT reservation block and `/reserved-memory`,
  the source storage, the EFI tables and the Malta PROM block.
- Loader entry contract: x86 low 1 MiB and fixed BIOS payload pad, Arm RAM below
  the image, PPC RAM below the image, the LoongArch command-line page.
- Kernel: the complete linked image `[_kernel_start, _kernel_end)` and the boot
  stack and page tables `[_kernel_bootstrap_start, _kernel_bootstrap_end)` as
  separate loaded objects (physical bounds; MIPS subtracts KSEG0).
- Physical page zero on every profile.
- Device windows: x86 IOAPIC/LAPIC/firmware ROM, Arm virt `[0, 1 GiB)`, RISC-V
  virt platform devices, LoongArch I/O and PCIe windows, e500 PCI window.

## Kernel map and allocator

`kernel_physical_map` converts the qualified managed ranges into a normalized
`memory::PhysicalMemoryMap` tagged with its source (`e820`, `fdt`,
`board_table`). `Kernel::boot` requires a `product_grade()` map in freestanding
builds and fails with `not_found` when none was discovered. It calls
`MemoryManager::initialize(map, page_size, direct_offset)`, which carves the
frame-allocator metadata from the top of the highest managed range. Frame
addresses are physical except on MIPS, where the KSEG0 offset is added.

The native runtime no longer discovers memory or owns a linker arena in kernel
builds. Its page pool activates over the kernel allocator with an explicit
kernel-object reserve and a user-page quota of every frame free at activation
outside that reserve. Per-frame registry state lives in each frame's
`PageDescriptor`; the former 1024-entry tables are gone. Hosted model builds keep
a separate host-backed arena.

## Diagnostics

```
OK_BOOT_MEMORY version=1 protocol=P source=S status=pass image_begin=X image_end=X direct_offset=X managed_pages=N managed_ranges=N excluded=N
OK_BOOT_MEMORY_SOURCE base=X size=X decoded=X
OK_BOOT_MEMORY_LOADER base=X size=X          (EFI tables)
OK_BOOT_MEMORY_BOARD reported_low=X reported_total=X   (Malta)
OK_BOOT_MEMORY_LOADED base=X size=X ram=0|1
OK_BOOT_MEMORY_RANGE base=X size=X
OK_BOOT_MEMORY_EXCLUDED base=X size=X reason=reserved|unmapped|address-width|page-edge|capacity
```

A failed discovery prints `status=fail message=...` and the kernel boot stops.
`OK_NATIVE_FRAMES` moved to `version=3`: `total` and `free` now describe the
shared kernel allocator and `user_limit` the activation quota. The former
`OK_NATIVE_MEMORY stage=prepare ... limit=1024` and `OK_NATIVE_MEMORY_RANGE`
lines are replaced by the lines above; `OK_ARM32_ENTRY` is unchanged.

## Verification

```sh
python3 scripts/test_boot_memory.py --sanitize
python3 scripts/test_physical_map.py --sanitize
python3 scripts/test_frame_allocator.py --sanitize
python3 scripts/test_firmware_handoff.py --sanitize
python3 scripts/test_qualified_memory.py
python3 scripts/test_native_page_pool.py
python3 tests/host/firmware_handoff_acceptance_tests.py
xmake test    # each of the ten profiles, debug, pinned QEMU 10.2.0
```

`test_boot_memory.py` covers the UEFI system table and PROM decoders, the
multi-step LoongArch discovery and its failure atomicity, Malta board
qualification (page zero, PROM page, image, high RAM outside KSEG0) and the
E820/FDT kernel plans and maps. The isolated actual-QEMU probe
`scripts/test_native_firmware_handoff.py` now accepts the LoongArch EFI read
sequence and the Malta board evidence; `--toolchain-dir` and `--qemu-dir` select
the pinned SysToolchain. Its saved transcripts for LoongArch (EFI) and both
Malta profiles come from QEMU 10.2.0.

All ten debug kernels boot on their discovered maps under the pinned QEMU and
pass `xmake test`. This is QEMU execution of the discovery path and allocator
bookkeeping over real RAM. It does not qualify physical machines, other loaders,
ACPI, hot-plug, DMA ownership, or RAM outside the managed windows.
