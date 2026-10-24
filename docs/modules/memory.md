# Memory Management

`ok::memory::FrameAllocator` provides fallible, bounded physical frame ownership
bookkeeping. It does not discover RAM, install page tables, pin DMA memory or
invalidate TLBs. The native architecture backends and the software mapping models
have separate execution and lifetime requirements.

## Bootstrap frame allocation

`initialize(regions, page_size)` accepts an unordered, potentially overlapping
map of half-open byte ranges. The page size must be a nonzero power of two and
all range endpoints must fit in `uptr`. Zero-length regions are ignored. Usable
regions form a union; every non-usable region takes precedence regardless of map
order. A page is eligible only if every byte belongs to usable memory and no byte
is reserved. Adjacent usable fragments are joined before alignment, duplicate
regions do not duplicate frames, and holes consume no frame metadata slots.

Initialization sweeps region boundaries into at most 64 eligible page ranges.
The fixed metadata supports 16,384 eligible pages (64 MiB with 4 KiB pages).
Larger maps return `StatusCode::overflow`; no arbitrary region is chosen and no
usable tail is silently discarded. `total_frames()` counts eligible pages,
`free_frames()` subtracts current ownership, and both start at zero. Failed
initialization preserves the previous allocator. A live allocation prevents
reinitialization with `StatusCode::busy`.

Each successful allocation returns a `PhysicalFrame` containing its address,
index, allocator identity and generation. Release checks all four fields. A
foreign handle, changed address/index, double release or stale handle after slot
reuse is rejected without changing accounting. The generation sequence survives
reinitialization and exhaustion fails instead of wrapping to a reusable value.
The allocator is immovable, must outlive its handles and requires externally
serialized calls. This does not enforce mapping references or protect against a
malicious kernel caller fabricating a complete valid handle. Callers must retire
all translations, references and I/O pins before releasing a physical frame.

This is the first allocation foundation under the
[Kernel allocation contract](../DEVELOPMENT.md#kernel-allocation-contract).
Runtime-sized metadata, per-owner quotas, reclamation, IRQ/SMP synchronization,
reference retirement and a production page/object allocator remain required.

## Boot map and VM scope

`Kernel::boot` checks that a supplied map fits `KernelConfig::memory_map` before
constructing its span. The current zero-region fallback is a synthetic 64 MiB
map beginning at 1 MiB; `kernel_main` still supplies no discovered firmware map.
That fallback is usable only for bring-up bookkeeping. It does not reserve the
kernel image, boot stack, page tables, firmware or devices, and its addresses
must not back new hardware mappings or DMA. Real E820/FDT/ACPI/platform RAM and
reservation discovery is an outstanding native boot gate on each architecture.

`LinearAddressSpace`, `PageTable` and `UserAddressSpace` in this module are
software mapping/copy models used by debug tests. `TranslationMode` records the
selected profile (`linear`, `paged`, `higher_half`); selecting a value does not
activate a hardware address space. Architecture page-table operations should use
constrained templates/static dispatch rather than a virtual interface solely to
share hot-path implementation.

The native user runtime also provides a [bounded anonymous VM slice](../NATIVE_VM.md)
using its separate linker-reserved backing arena. Its hardware mappings and
acceptance tests are distinct from this module's software VM models.

## Verification

```sh
python3 scripts/test_frame_allocator.py
python3 scripts/test_frame_allocator.py --sanitize
```

The hosted suite tests reserved overlap, sparse/duplicate/adjacent regions,
unaligned bounds, full capacity and exhaustion, initialization rollback, address
wrap, foreign/stale release and a deterministic byte-coverage oracle over 200
maps in both input orders. The shared freestanding checks are also reached by
`test_memory` through `kernel_main` in debug builds and `xmake qemu-test`. These
are allocator bookkeeping checks even when run in QEMU; they do not establish
real RAM discovery, hardware mappings or process isolation.
