# Qualified entry memory

`boot::qualify_boot_memory` is a bounded, failure-atomic normalization layer
between owned firmware discovery and a future physical allocator. It does not
initialize `FrameAllocator`, replace the native runtime page pool, or establish
physical-machine/SMP support.

## Ownership and rejection rules

The caller supplies a stable, successfully discovered `FirmwareMemory`, stable
loaded-object reservations, an explicit mapping/physical-address policy, and
distinct workspace/output objects. The output owns the full original source,
reservations, policy, sorted page-aligned managed ranges, and all excluded bytes
of the usable-RAM union. `valid()` establishes normalization only; it is not
permission to turn physical integers into pointers. The kernel caller remains
responsible for the provenance and lifetime of its discovered source.

Reservations win over usable RAM. Disabled/unknown E820 entries and all FDT
reservations remain excluded. The full firmware source storage is reserved,
including unused E820 table capacity. Loaded RAM must lie wholly inside the
usable union and may not intersect firmware reservations or source storage.
Loaded claims may overlap each other, such as a stack inside a complete image;
the union is reserved. A reservation with `require_usable_ram=false` is only an
exclusion (for example low loader scratch/MMIO), not proof that the bytes are RAM.

The normalizer subtracts reservations before alignment. It next excludes bytes
outside the explicit accessible window, outside the address ceiling, and at
partial page edges. Its final explicit capacity policy either requires all
remaining RAM or retains the lowest addresses. Exclusion priority follows this
order; each source usable byte belongs to exactly one managed/excluded range.
Ranges use checked `u64` exclusive ends, including on 32-bit targets. The one
unrepresentable exclusive endpoint 2^64 is rejected instead of wrapping.

There are at most 64 supplied reservations and 512 managed/exclusion records.
Range fragmentation or diagnostic exhaustion fails without changing output,
even under the subset policy; no incomplete exclusion list is published. The
hosted suite exercises exactly 512 exclusions and the first failing additional
record. Input/output/workspace aliases that would destroy inputs are rejected;
output-backed loaded spans remain readable until final publication. Static
workspace/output storage is suitable at boot. Current compiler reports put the
largest normalization stack frame around 25 KiB, with approximately 8 KiB nested
helpers; this is an entry-stack operation, not an IRQ-stack operation.

## Concrete QEMU entry policy

`qemu_entry_memory_plan<A>` uses static architecture dispatch and requires the
matching captured protocol and physical linker image bounds. It applies only
before the normal kernel VM initialization changes entry mappings. It is not a
general physical-to-virtual translator. External archives/modules/initrds must
be supplied as additional loaded reservations; the isolated probe loads none.

| Profile | Actual entry mapping restriction | Additional reserved extent |
| --- | --- | --- |
| x86_64 | Initial boot page tables identity-map the first 1 GiB | Low 1 MiB; complete fixed BIOS payload through 5 MiB |
| i386 | BIOS flat protected mode, paging off; explicit 4 GiB ceiling | Low 1 MiB; complete fixed BIOS payload through 5 MiB |
| aarch64 | Linux Image entry at EL1, MMU off; qualified QEMU RAM window 0x40000000–0x60000000 | Prefix before image at 0x40080000 |
| rv64/rv32 | `-bios none`, bare `satp`, MPRV clear; qualified QEMU RAM window 0x80000000–0xa0000000 | Complete loaded image |
| ppc | Captured ePAPR `r7` initial mapping extent | Prefix before image at 0x01000000 |
| arm32/loongarch64 | Direct ELF has no discovered firmware map | No qualification or RAM writes |
| mips/mips64 | Malta PROM is not a complete decoded firmware map | No qualification or RAM writes |

All profiles reserve `_kernel_start.._kernel_end`, including linked text/data,
BSS, bootstrap tables, and the NOLOAD boot stack. The x86 padded BIOS reservation
is extended if the actual linked image exceeds it. Image bounds must match the
loader contract and fit within the accessible entry window. Fixed windows above
limit access; usable RAM still comes exclusively from the actual firmware map.
The full high-RAM source is retained even when entry mappings cannot reach it.

## Executable evidence

The owning Kernel runners require no OS checkout or SDK:

```sh
python3 scripts/test_qualified_memory.py --sanitize
python3 tests/host/qualified_memory_acceptance_tests.py
python3 scripts/test_native_qualified_memory.py --output build/qualified-memory
python3 scripts/test_native_qualified_memory.py --output build/qualified-high --arch i386 --arch x86_64 --x86-high
python3 scripts/test_native_qualified_memory.py --output build/qualified-el2 --arch aarch64 --aarch64-el2
python3 scripts/test_native_qualified_memory.py --output build/qualified-short --arch i386 --arch x86_64 --arch aarch64 --arch rv64 --arch rv32 --arch ppc --header-only
```

Use `--kernel-root` when running from a staged patch. The native runner uses the
pinned compiler/flags inventory of the typed firmware runner, freezes its input
sources, records commands/compiler/QEMU hashes, and retains the exact ELF,
image, serial output and result receipt. Every build is independent of the
shared xmake output directory.

The hosted suite checks 1,000 randomized maps against independent page ownership
and complete interval-partition oracles, plus capacities, overflow, aliases,
nested reservations and failure atomicity. The portable serial parser compares
native managed pages and each exclusion reason with independent firmware range
subtraction and rejects incomplete, duplicated, reordered or fabricated evidence.
Its fixtures are regression data, not new native executions.

The native probe executes the real architecture entry assembly, decodes the
captured firmware, reserves the full linker image, and writes/reads/restores the
first and last word of each of 1,024 qualified pages. It checks x86/AArch64/RISC-V
entry control state before writes. Four unavailable contracts and shortened
source windows publish zero managed pages and perform zero RAM writes. This
proves actual entry RAM access for the exercised subset. It does not prove
allocator ownership, general mappings, process isolation, DMA, concurrency, or
support for hardware beyond the named QEMU recipes.

## Next allocator boundary

1. Add a checked adapter from a valid qualified map to `FrameAllocator` only
   after choosing that allocator's explicit 16,384-page/64-range capacities.
   Verify every `u64` range is representable by its current `uptr`/`usize` API,
   including its exclusive end; a 32-bit end at exactly 4 GiB is not currently
   representable by that API. Do not silently truncate the source or cast it.
2. Keep firmware source/loaded reservations and the selected mapping policy
   owned throughout allocator lifetime. Actual zero/copy access must go through
   a translator tied to the current architecture page-table lifetime. The entry
   policy above expires when normal VM initialization replaces those mappings.
3. First exercise a separate serialized native allocator instance: exhaust and
   reclaim every qualified page, reject stale/foreign handles, retain poisoned
   ownership across failures, and verify kernel/source sentinels remain intact.
   Reserve allocator metadata and all native runtime page/table arenas before
   admitting either to a common pool.
4. Switch the production runtime only after the allocator, mappings, all loaded
   objects and release/TLB/DMA lifetimes form one reviewed ownership contract.
   Four missing firmware protocols, runtime-sized metadata, charges/reclaim,
   general direct mappings and SMP remain separate mandatory work.
