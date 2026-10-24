# Boot memory parsing and remaining handoff work

All ten assembly entry paths preserve protocol-relevant arguments in the owned
`RawBootHandoff` record after BSS initialization. The shared x86 BIOS loader
collects a bounded E820 table at physical `0x5000`; its parser is available in
`ok/boot/e820.hpp`. Capture and parsing do not qualify a native allocator map.

`ok/boot/fdt.hpp` and `src/boot/fdt.cpp` implement an isolated freestanding FDT
memory parser. The FDT source is not wired into boot or the build. Neither
firmware parser is called by normal Kernel startup yet. The existing synthetic
bookkeeping default in `Kernel::boot` and separate linker-reserved native page
pool remain unchanged. Parser tests do not establish safe physical allocation.

## Raw capture and BIOS E820 contract

`ok_boot_capture` runs on the primary CPU after BSS clearing and ABI stack setup.
Its owned record stores x86 EAX/EBX followed by two zeros; PPC r3/r6/r7 followed
by zero; and the first four argument registers on the remaining profiles. The
`captured` flag only proves that the hook ran. Zero arguments are not a memory
map. Consumers must finish platform/protocol identification before interpreting
words as pointers; secondary CPUs cannot race initial capture.

The BIOS collector uses INT 15h E820 with SMAP signature, ES:DI storage, a
24-byte request and an EBX continuation token. It preserves DS, ES, EBP and EDI
across firmware calls. Exact 20-byte replies gain enabled attributes; 24-byte
replies preserve attributes. Unknown types, disabled and zero-sized records
are retained. A zero continuation token or carry after a valid record completes
enumeration; carry on the first call fails. Invalid signature/size fails; 128
records with a continuation token report capacity exhaustion. Failed/full
headers retain the validated partial count for diagnostics, but the parser
rejects them and leaves output unchanged.

The table is a 16-byte little-endian header (magic `0x30323845`, status, count,
record size), followed by at most 128 records of 24 bytes. Status 1 is complete,
2 failed, 3 full. The parser validates exact header/count/size bounds and
64-bit range arithmetic, then copies every record into owned storage. Startup
must reserve the complete `[0x5000,0x5c10)` table before allocation and reserve
other loaded/boot objects separately. The 512-byte boot sector and existing
fixed 4 MiB payload loading scheme are preserved. Actual incoming Multiboot
EAX/EBX values remain distinct; this change implements no Multiboot decoder.

## Parser contract

`parse_fdt_memory(blob, output)` accepts an already mapped, explicitly bounded
FDT v17 byte span. It validates magic, totalsize, version compatibility, aligned
structure/reservation offsets, disjoint blocks, string bounds, big-endian
tokens, property ordering and balanced nodes. Failure leaves all of `output`
unchanged. Results own values; they retain no firmware pointers.

It returns the enabled root memory nodes' `reg` tuples using root
`#address-cells` and `#size-cells`. Either width may be one or two cells; absent
root properties use the FDT defaults of two address cells and one size cell.
The 64-bit reservation table and static `/reserved-memory` child ranges become
reservation records with source provenance. `no-map` and `reusable` attributes
are preserved. Overlaps and duplicate records are retained for the downstream
allocator's reservation subtraction; parsing does not claim they describe a
valid board configuration.

`/reserved-memory` must explicitly match the root cell widths and have empty
`ranges`. Nonidentity translations, memory below another bus, nested reserved
buses and enabled dynamic reservations without `reg` are unsupported. Disabled
nodes are not published. Static `reg` takes precedence over dynamic allocation
hints, as specified by the reserved-memory binding. Zero-length and wrapping
physical ranges fail. Address cells are never truncated to the kernel's pointer
width, including on 32-bit architectures.

Bounds are explicit: a 2 MiB blob, 32 node levels, 64 RAM tuples and 128 total
reservation tuples. Any capacity excess fails without partial publication.
This is a RAM/reservation parser, not a general device-tree validator, bus
translator, initrd loader, allocator or ACPI/UEFI implementation. In particular,
platform policy must separately handle properties such as a restricted usable
memory window and reserve any boot objects absent from the firmware table.

## Six QEMU device-tree audits

The installed QEMU 8.2.2 machine `dumpdtb` option produced these records using
the harness's `-m 256M` profiles, except LoongArch's `-m 2G`. All six trees use
two address and two size cells. The dumps contain no reservation-table entries
or `/reserved-memory` ranges; this does not mean the loaded kernel, DTB or boot
objects are free.

| Profile | Parsed RAM address | Parsed byte size |
| --- | --- | --- |
| aarch64 | `0x40000000` | `0x10000000` |
| arm32 | `0x40000000` | `0x10000000` |
| rv64 | `0x80000000` | `0x10000000` |
| rv32 | `0x80000000` | `0x10000000` |
| ppc | `0` | `0x10000000` |
| loongarch64, `/memory@90000000` | `0x290000000` | `0x270000000` |
| loongarch64, `/memory@0` | `0x200000000` | `0x210000000` |

The LoongArch values are an actual malformed board description, not a parser
endianness error. QEMU v8.2.2's
[`fdt_add_memory_node`](https://github.com/qemu/qemu/blob/v8.2.2/hw/loongarch/virt.c#L295)
calls `qemu_fdt_setprop_cells(..., 2, base, 2, size)` instead of the sized-cells
helper. The literal `2` becomes each field's high word. Both a separate byte
decoder and this parser reproduce the same values. Integration must reject
this map, qualify a fixed QEMU version, or validate QEMU's separate `etc/memmap`
fw_cfg data. Masking off the upper words would conceal a firmware error and is
not implemented.

## Handoff gaps for all ten profiles

| Profile | Current boot source and information | Missing handoff |
| --- | --- | --- |
| x86_64 | BIOS E820 table tag/pointer are captured; optional incoming Multiboot identity is retained | Validate the complete E820 source and loaded objects, then qualify RAM against mapping/allocator limits. Multiboot decoding remains unsupported. |
| i386 | Same BIOS table and original EAX/EBX capture | Same qualification gap; preserve 64-bit physical source ranges despite 32-bit pointer/mapping limits. |
| aarch64 | x0-x3 are saved before CurrentEL/EL transition and captured after BSS | Validate/map/reserve the Linux Image DTB pointer from x0 and qualify its RAM. |
| arm32 | SDK 13 `arm32-linux-image` at `0x40010000` supplies genuine r2 FDT with r0=0/r1=0xffffffff | Capture/validate actual ARM SVC, boot CPU and MMU/cache state before bounded FDT access. Reserve full source storage, 64 KiB prefix and full image; runtime aperture is 256 MiB. Historical direct ELF remains a negative. |
| rv64 | a0-a3 are captured, including ROM hart ID/DTB inputs | Bound/map/reserve the authoritative a1 DTB and qualify its RAM. |
| rv32 | Same raw capture with a 32-bit DTB pointer | Same gap; retain 64-bit parsed tuples and reject unrepresentable mappings explicitly. |
| loongarch64 | a0-a3 are captured; current direct ELF arguments are zero | A board-defined tree exists at physical 1 MiB, but its erroneous RAM high cells described above block qualification. No synthetic or masked map is provided. |
| mips | a0-a3 capture preserves the Malta PROM inputs | A bounded PROM parser must use real `memsize`/`ememsize` semantics, 32-bit table entries and reserved PROM storage. This is not FDT boot. |
| mips64 | Same capture under n64 with sign-extended PROM addresses | Decode firmware 32-bit pointer entries with checked KSEG translation; do not read native-width table slots. |
| ppc | r3/r6/r7 are saved before MMIO/BSS scratch use and captured | Validate ePAPR magic/mapped extent, then map/parse/reserve the image-dependent r3 DTB. Never hardcode an observed pointer. |

The ARM distinction is confirmed by QEMU's
[`arm_setup_direct_kernel_boot`](https://github.com/qemu/qemu/blob/v8.2.2/hw/arm/boot.c#L952)
and its
[DTB size-limit check](https://github.com/qemu/qemu/blob/v8.2.2/hw/arm/boot.c#L547).
RISC-V's
[reset vector](https://github.com/qemu/qemu/blob/v8.2.2/hw/riscv/boot.c#L381)
loads a1 from its calculated DTB address; with 256 MiB at `0x80000000`, its
2 MiB-aligned placement is `0x8fe00000`, but the register remains authoritative.
LoongArch's
[direct reset](https://github.com/qemu/qemu/blob/v8.2.2/hw/loongarch/virt.c#L674)
only sets PC and its
[FDT installation](https://github.com/qemu/qemu/blob/v8.2.2/hw/loongarch/virt.c#L878)
uses physical 1 MiB. Malta's
[register contract](https://github.com/qemu/qemu/blob/v8.2.2/hw/mips/malta.c#L728)
and
[PROM layout](https://github.com/qemu/qemu/blob/v8.2.2/hw/mips/malta.c#L63)
place a 16-entry table plus 256-byte string slots at physical `0x2000`, totaling
4,160 bytes. PPC's
[reset handoff](https://github.com/qemu/qemu/blob/v8.2.2/hw/ppc/e500.c#L788)
provides the ePAPR values before this kernel's assembly overwrites them.

## Smallest next integration boundary

Build a protocol-tagged boot source from the owned raw arguments, then obtain
an explicitly mapped/bounded source window. Keep firmware parsing, platform
qualification and allocator ownership as separate typed states. Unknown,
missing, malformed or incomplete sources must return explicit errors; never
manufacture a fallback physical map. Keep each platform's address translation
explicit: MIPS KSEG aliases are virtual addresses, not physical RAM ranges.

For x86, validate and parse the BIOS collector table during native startup.
Reserve the boot sector, BIOS transfer buffer, temporary stacks, table and
fixed payload load extent until their lifetimes end. Multiboot, if supported
as a separate path, needs a tagged and independently validated memory-map/module
handoff; the existence of the header does not populate that information.

Before frame allocation, combine the exact firmware records with reservations
for `_kernel_start.._kernel_end` including NOLOAD stacks/page tables, the loaded
OS archive, firmware tables, DTB, initrd and platform-specific boot buffers.
Verify that every loaded RAM object lies in a qualified RAM range. Preserve
`no-map`; do not automatically release reusable ranges. Firmware map parsing
must not manufacture a fallback region after failure.

Two existing bounds also prevent transparent wiring today: `KernelConfig` has
only eight memory-map records, and `FrameAllocator` tracks at most 16,384 usable
frames (64 MiB at 4 KiB pages). Real 256 MiB/2 GiB maps exceed that frame bound.
The integration must deliberately expand the allocator's metadata or define an
explicit managed-memory limit while retaining the full physical map; it must
not silently truncate firmware records to make initialization pass.

## Validation

Run `python3 scripts/test_fdt_memory.py`; add `--sanitize` for ASan/UBSan and
repeat `--dtb /absolute/path/file.dtb` to inspect explicit firmware fixtures.
Tests cover all one/two-cell combinations, FDT defaults, disabled nodes,
reservation provenance, ranges above 4 GiB, source-buffer lifetime, every
truncation of a valid tree, malformed offsets/tokens/strings, translation
rejection, capacity failures, wrapping ranges and deterministic hostile-byte
mutations. Every rejected input checks unchanged output. The parser also
compiles with all ten freestanding cross-compilers. These remain parser and
source-contract checks; native RAM handoff and allocator execution require
separate boot and OS SDK-consumer acceptance after integration.

Run `python3 scripts/test_e820_memory.py` (optionally `--sanitize`) for hosted
E820 parser checks. These cover full 64-bit address/size values, overlapping
reservations, disabled/unknown/zero-length records, malformed headers/counts,
capacity, wrapping ranges, every truncation and unchanged failure output.
Actual BIOS collector execution, raw capture and allocator qualification need
separate native evidence; a parser pass does not establish those outcomes.
