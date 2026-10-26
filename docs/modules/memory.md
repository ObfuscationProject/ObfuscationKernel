# Memory Management

`ok::memory::FrameAllocator` provides fallible, bounded physical frame ownership
bookkeeping. It does not discover RAM, install page tables, pin DMA memory or
invalidate TLBs. The native architecture backends and the software mapping models
have separate execution and lifetime requirements.

## Frame allocation

`initialize(regions, page_size, metadata)` accepts an unordered, potentially
overlapping map of half-open byte ranges. The page size must be a nonzero power
of two and all range endpoints must fit in `uptr`. Zero-length regions are
ignored. Usable regions form a union; every non-usable region takes precedence
regardless of map order. A page is eligible only if every byte belongs to usable
memory and no byte is reserved. Adjacent usable fragments are joined before
alignment, duplicate regions do not duplicate frames, and holes consume no
frame metadata.

Metadata is runtime-sized and caller-owned: a range table, a free bitmap and one
16-byte `PageDescriptor` per eligible frame. `metadata_bytes(regions, page_size)`
returns the exact requirement and `metadata_bytes_for(frames, ranges)` the bound
for a known shape; `FrameMetadataStorage<Frames, Ranges>` is a correctly aligned
static buffer for models and fixed arenas. Storage smaller than the requirement
returns `StatusCode::no_memory`; no usable tail is silently discarded and no
arbitrary region is chosen. Every check precedes the first metadata write, so a
failed initialization preserves the previous allocator. A live allocation
prevents reinitialization with `StatusCode::busy`.

Each successful allocation returns a `PhysicalFrame` containing its address,
index, allocator identity and generation. Release checks all four fields. A
foreign handle, changed address/index, double release or stale handle after slot
reuse is rejected without changing accounting. The generation sequence survives
reinitialization and exhaustion fails instead of wrapping to a reusable value.
`descriptor(frame)` returns the descriptor of a live exact handle; the allocator
clears its reference, pin, kind and flag fields on every allocation and never
interprets them. `index_of(address)` and `handle_at(index)` map between frame
addresses and live handles. `allocate(account)` and `release(frame, account)`
charge a `ChargeAccount` (owner, limit, peak) before a frame is handed out; a
quota failure is distinct from exhaustion and leaves both unchanged.
`allocate_contiguous(count, ...)` returns physically adjacent frames within one
range for metadata and buffers that need a single span.

`PhysicalFrame::address` is the supervisor direct-map address: the physical
address on every profile except MIPS, where it is the KSEG0 alias the kernel and
the execution backend already use.

The allocator is immovable, must outlive its handles and requires externally
serialized calls. This does not enforce mapping references or protect against a
malicious kernel caller fabricating a complete valid handle. Callers must retire
all translations, references and I/O pins before releasing a physical frame.
Reclamation, reserves, OOM policy and IRQ/SMP synchronization remain required by
the [Kernel allocation contract](../DEVELOPMENT.md#kernel-allocation-contract).

`MemoryManager::initialize(map, page_size, direct_offset)` is the boot form: it
carves the metadata from the top of the highest usable range of a normalized
`PhysicalMemoryMap`, reserves that carve and manages every other usable page.
The map must contain only RAM the supervisor can already access.
`metadata_region()` reports the carve. The model form takes caller storage.

## Kernel objects

`SlabAllocator` (`ok/memory/slab.hpp`) is the general kernel object allocator.
Objects up to 1024 bytes use the power-of-two classes 16..1024 in slab pages
(252 slots of 16 bytes down to 3 of 1024); larger objects take physically
contiguous frames. Objects are zeroed and aligned to their class (large objects
to a page). Each slab page holds its partial-list link, free count and slot
bitmap in a 64-byte header, and is identified by its `PageDescriptor` (kind
`kernel_slab`, class in `flags`, live objects in `references`); large runs are
kind `kernel_large` with the page count on the head.

It follows the allocation contract below:

- Owner: the `MemoryManager`; `initialize_objects()` runs after the boot form and
  refuses the model form, whose frames are never dereferenced.
- Accounting: every held frame charges the allocator's page account, limited to
  a quarter of the managed frames; every object charges its caller's
  `ChargeAccount` in bytes of its class (or pages for large objects).
- Allocation context: `boot`, `kernel` and `atomic`, all on the serialized boot
  CPU. Only `atomic` may take frames from the reserve of 1/128 of the managed
  frames (16..256) that the others must leave free. Nothing blocks yet.
- Reclamation: a slab page whose last object is released returns to the frame
  allocator unless it is its class's only partially used page; large objects
  return their frames on release. Failure returns `no_memory` and counts in
  `accounting().failures`; there is no OOM killer.

The node-owned [page cache skeleton](filesystem.md#page-cache-skeleton) draws its
pages from the same allocator (kind `page_cache`) and its index from this slab.

Release checks the page kind, class, slot alignment, slot occupancy and the
owner's charge, so foreign, misaligned, wrong-size and double releases fail
without changing state. Slot reuse inside one page is not detected; callers that
need stale-handle detection keep using the generation-checked `ObjectPool`.

## Boot map and VM scope

`kernel_main` discovers the boot memory map once, before translations change,
and passes it to `Kernel::boot` through `KernelConfig::physical_memory`. All ten
profiles have a loader contract: BIOS E820 (x86), FDT (aarch64, arm32, RISC-V,
PPC), a UEFI system table naming an FDT (LoongArch) and the Malta board table
sized by the PROM environment (MIPS). Firmware reservations, the loader tables,
the linked image, the boot stack and page tables, page zero and the platform
device windows are reserved explicitly; RAM outside the profile's managed window
is recorded as excluded. See [discovered boot memory map](../qualification/BOOT_MEMORY_MAP.md).

A freestanding `Kernel::boot` without a product-grade map fails with
`not_found`. The synthetic 64 MiB map starting at 1 MiB exists only in hosted
model builds, whose frame addresses are never dereferenced.

`LinearAddressSpace`, `PageTable` and `UserAddressSpace` in this module are
software mapping/copy models used by debug tests. `TranslationMode` records the
selected profile (`linear`, `paged`, `higher_half`); selecting a value does not
activate a hardware address space. Architecture page-table operations should use
constrained templates/static dispatch rather than a virtual interface solely to
share hot-path implementation.

The native user runtime also provides a [bounded anonymous VM slice](../qualification/NATIVE_VM.md)
and, behind `native_cow_fork`, [copy-on-write fork](../qualification/NATIVE_COW_FORK.md).
In kernel builds its page pool draws from the kernel allocator; hosted model
builds keep a host-backed arena. Its hardware mappings and acceptance tests are
distinct from this module's software VM models.

## Verification

```sh
python3 scripts/test_frame_allocator.py
python3 scripts/test_frame_allocator.py --sanitize
python3 scripts/test_physical_map.py
python3 scripts/test_physical_map.py --sanitize
python3 scripts/test_slab.py --sanitize
python3 scripts/test_boot_memory.py --sanitize
```

The hosted suites test reserved overlap, sparse/duplicate/adjacent regions,
unaligned bounds, maps larger than the former fixed table, many-range maps,
undersized metadata, exhaustion, initialization rollback, address wrap,
foreign/stale release, charged allocation and quota failure, contiguous runs,
descriptor reset, the boot metadata carve over host-backed RAM and a
deterministic byte-coverage oracle over 200 maps in both input orders. The slab
suite covers every class, release validation, reclamation, page and owner
limits, the atomic reserve, large objects and the boot policy.

In debug kernels the `memory/discovered-ram-frames-and-objects` test point
writes and reads back a two-frame run and one object of each class plus two
large objects through the booted kernel's allocator, on every profile under
QEMU. It touches real guest RAM through supervisor addresses; it does not
qualify user mappings or DMA. The shared freestanding checks are also reached by
`test_memory` through `kernel_main` in debug builds and `xmake qemu-test`. These
are allocator bookkeeping checks even when run in QEMU; they do not establish
real RAM discovery, hardware mappings or process isolation.
