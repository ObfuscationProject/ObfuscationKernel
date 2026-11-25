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

The allocator is immovable and must outlive its handles. Any CPU may call it
with interrupts masked, outside interrupt top halves: its own lock (rank
`frame`, the innermost memory lock, `ok/smp/lock.hpp`) covers the bitmap, the
counts, the generation sequence and each descriptor's generation, and
`descriptor()` checks the exact handle under it. A descriptor's other fields
belong to the frame's current owner, which changes them under its own lock. The
used count is an atomic word read without the lock. This does not enforce
mapping references or protect against a malicious kernel caller fabricating a
complete valid handle. Callers must retire all translations, references and I/O
pins before releasing a physical frame. Reclamation, reserves and OOM policy
remain required by the
[Kernel allocation contract](../DEVELOPMENT.md#kernel-allocation-contract).

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
- Allocation context: `boot`, `kernel` and `atomic`, on any CPU with interrupts
  masked, outside interrupt top halves; one lock (rank `slab`) covers the class
  lists, the slab headers, the page account and the counters, with the frame
  lock inside it. Only `atomic` may take frames from the reserve of 1/128 of the
  managed frames (16..256) that the others must leave free. Nothing blocks yet.
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

The software mapping/copy models (`LinearAddressSpace`, `PageTable`,
`UserAddressSpace`, `VirtualMemoryManager`) live with the kernel's roadmap tests
and test points in `tests/kernel/model/vm_model.hpp`; this module keeps no fixed
user page or mapping tables. `TranslationMode` records the
selected profile (`linear`, `paged`, `higher_half`); selecting a value does not
activate a hardware address space. Architecture page-table operations should use
constrained templates/static dispatch rather than a virtual interface solely to
share hot-path implementation.

The native user runtime also provides a [bounded anonymous VM slice](../qualification/NATIVE_VM.md)
and [copy-on-write fork](../qualification/NATIVE_COW_FORK.md), with eager fork copies
behind `--native_cow_fork=n`.
In kernel builds its page pool draws from the kernel allocator; hosted model
builds keep a host-backed arena. Its hardware mappings and acceptance tests are
distinct from the software VM models.

## Locks and user-access windows

Every allocator has its own lock at its global rank (`ok/smp/lock.hpp`), so
the memory layer is safe for callers that do not hold the big kernel lock
(smp-scaling WP3; today every caller still holds it):

| Rank | Lock | Covers |
|---|---|---|
| 60 | `entry_stack` | `KernelEntryStackPool` records and per-CPU active slots; a slot is reserved under it, its canaries written and its backing cleared outside it |
| 62 | `mm_retire` | the retirement quarantine's slot state words; a slot is popped (reserved or claimed busy) under it and its tables and frames are retired after unlocking |
| 64 | `arch_spaces` | a backend's address-space slots, space ids and TLB generation bumps |
| 66 | `shared_files` | the memfd file table, page indexes and retirement ledger; sizing stages its frames outside it |
| 68 | `object_pool` | one `ObjectPool`'s pages, slot generations and counters |
| 70 | `page_pool` | `NativePagePool` quotas, counters and the reference, pin and flag fields of native descriptors; object runs are reserved under it and zeroed after |
| 72 | `slab` | the slab allocator |
| 74 | `frame` | the frame allocator |

Each process's address space has a class B gate, `ProcessResources::mm` (rank
`mm`): VM records, the program break, the memory limits and the publication of
`native_space` (a seqcount) change under it, and translation writers (remap,
protect, destroy) of a published space run holding it. mmap stages its frames,
shared-file aliases and file bytes before taking mm and then builds its
candidate from the records as they are, so a sibling's change meanwhile is
kept.

User translations are reached through windows (`ok/smp/bringup.hpp`). A user
entry opens a user window, and every kernel user access (`copy_*_user`,
`user_range`, `user_word`) a kernel window, before it validates its space in
the slot table; the writer publishes the new entry (or marks the slot
retiring), bumps the TLB generation, fences and calls `quiesce_root`, which
kicks user windows and waits for kernel windows, before anything the old entry
reached is freed. Backends never call back into the runtime: a page whose write
access is withheld (copy-on-write) is refused, and `ok/user/uaccess.hpp`
resolves it under mm and retries, with a final attempt holding mm.

## Verification

```sh
python3 scripts/test_frame_allocator.py
python3 scripts/test_frame_allocator.py --sanitize
python3 scripts/test_physical_map.py
python3 scripts/test_physical_map.py --sanitize
python3 scripts/test_slab.py --sanitize
python3 scripts/test_boot_memory.py --sanitize
python3 scripts/test_translation_windows.py
```

The frame, slab, page pool, object pool and entry stack runners also build a
race model (`OK_HOSTED_LOCK_MODEL`, `OK_LOCK_DEBUG`) whose host threads stand in
for CPUs on the real locks; `test_translation_windows.py` races windows against
a writer whose frees are poisoned. Both are model coverage only.

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
