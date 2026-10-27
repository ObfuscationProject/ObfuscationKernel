# Bounded native virtual memory

The native user runtime handles `mmap`, `mprotect` and `munmap` before the trusted
POSIX syscall table. These calls modify the real execution backend's page tables
or TLB mapping descriptions. They never call the metadata-only `PosixService`
VM functions. All ten execution backends remain supported without a new runtime
virtual interface.

## Supported interface

- `mmap`: exactly one of `MAP_PRIVATE` and `MAP_SHARED`, optionally with
  `MAP_ANONYMOUS` and `MAP_FIXED`. Anonymous mappings require file descriptor
  `-1` and offset zero. `MAP_SHARED | MAP_ANONYMOUS` retains the same backing
  frames across `fork`; private mappings are shared copy-on-write
  ([NATIVE_COW_FORK](NATIVE_COW_FORK.md)), or copied eagerly with
  `--native_cow_fork=n`.
  `MAP_PRIVATE` of a readable regular file eagerly copies the requested pages,
  including nonzero page-aligned offsets and partial-EOF zero fill. It has no
  file-cache coherence. The length is nonzero and rounded up to 4096 bytes
  with overflow checks.
  A nonzero address is an optional hint rounded down to a page boundary; an
  occupied or invalid hint falls back to first fit. Ordinary allocation starts
  16 MiB above the architecture's user window base. Every new backing page is
  zeroed before publication.
  `MAP_FIXED` requires an exact page-aligned address in the user window and
  replaces existing backing atomically after final page-count admission.
- `brk`: the initial break is derived from admitted ELF segment ends. Growth
  maps zeroed read/write pages; shrink retires complete pages. Growth rejects
  collisions, including `PROT_NONE` mappings. Failed Linux raw `brk` returns
  the unchanged break; the native operation retains its explicit error.
- `mprotect`: page-aligned address, nonzero page-rounded length and a completely
  allocated range. Supported protection values are `PROT_NONE`, `PROT_READ` and
  `PROT_READ | PROT_WRITE`. A range containing a hole fails with `ENOMEM` without
  partially changing existing pages.
- `munmap`: page-aligned address and nonzero page-rounded length in the user
  window. Partial unmap splits existing mapped ranges. Already-unmapped pages
  are harmless, including a range spanning both mapped pages and holes.

Unknown flags, ambiguous private/shared selection, invalid anonymous fd/offset,
write-only and executable protection requests fail with `EINVAL`. Shared file
mappings are explicitly unsupported because there is no coherent page cache.
Private file mappings reject a complete page beyond EOF until a file-fault path
can deliver `SIGBUS`. Executable ELF mappings continue through the existing
loader. This interface does not add JIT execution or claim hardware NX on the
preserved MIPS profiles, whose CPUs lack execute-inhibit support.

## Ownership and replacement

Backing and mapping metadata are charged to the canonical 1024-frame native
`FrameAllocator` pool, which uses qualified firmware frames when supplied and
otherwise the linker-reserved arena. `NativePagePool` retains exact owner and
generation handles, a shared-frame classification and counted process references.
User pages cannot spend the kernel-object reserve. Retired page-table pins are
independent of process references; the last reference releases backing only
after all pins retire.
Hardware backends receive the same kernel-accessible backing addresses as the
ELF loader; the MIPS backend retains its existing alias-to-physical translation.

`PROT_NONE` pages keep their backing and contents in `native_pages`. The stable
chunked backend view includes their records, but each backend omits their user
translation. Restoring protection remaps the same contents. Fork shares
private pages copy-on-write (or copies them eagerly), including inaccessible
ones, and retains references to shared anonymous frames. Per-process
protection and unmap do not change a peer's translations. A shared frame remains classified as shared with one process
reference so a later fork still shares it.
Exit, exec replacement and process kill use the existing native release path.
That path retains backing when hardware table retirement fails.
If a metadata lease cannot be returned during final owner teardown, its lease
destructor transfers the exact frame generation to the page pool's sticky object
quarantine. The pool does not reallocate that frame. `OK_NATIVE_FRAMES` reports
`object_quarantine`, and `quarantined_kernel_page(index)` exposes the retained
canonical handle for diagnostics. Explicit `clear()` failure leaves the lease in
its mapping owner for correction or retry before that final teardown.

Each VM operation stages a fallibly allocated candidate page list and coalesced `MemoryMap`, then
creates a replacement hardware address space while the original remains live.
Only after creation succeeds does it retire the original and publish the new
space, ownership list and metadata. Unmap releases removed backing only after
retirement succeeds. Failed allocation, metadata capacity, table creation or old
table retirement preserves the original mappings and returns staged pages to
the pool. If a backend also fails to retire freshly created rollback tables,
staged pages are quarantined rather than reused through live translations; this
is a fail-closed exceptional path, not a reclamation policy.
One emergency quarantine record retains a candidate if a backend violates its
advertised 16-slot limit. Exhausting that final record, duplicating a retained
space owner or failing to pin referenced backing stops execution immediately;
returning would allow a borrowed mapping view or frame to be freed.
Fixed replacement keeps old pages allocated until their old translations retire.
Consequently replacement can fail for lack of temporary backing even when its
final page count fits the process limit.

All operations run serially on the boot CPU after `enter()` has returned to the
kernel address space. Existing architecture entry/return code performs its
required table activation and local TLB invalidation before another user task
runs. There is no remote TLB protocol or SMP lifetime proof here.

## Bounds and remaining work

The metadata sequence allows at most 1024 mapping records per process. On a
64-bit target, a 4096-byte metadata block holds 170 records, so one owner uses
at most seven blocks. Sixteen backend slots cover live and quarantined spaces;
one staged owner may exist before it receives a slot. Their worst-case metadata
charge is therefore 17 × 7 = 119 pages. The existing object pools require 38
pages on x86_64, for an explicit 157-page kernel reserve and an 867-page user
quota from a 1024-frame pool. On 32-bit targets a block holds 341 records and
the mapping charge is 17 × 4 = 68 pages; object-pool pages are calculated from
their architecture-specific object sizes at compile time. Activation rejects a
pool smaller than its reserve. The actual total allocation remains limited by
the available frames.

Sixteen coalesced `MemoryMap` areas and each architecture's fixed page-table
pool remain separate limits. Replacement needs one additional free native
address-space slot. Capacity failure is reported without destroying the previous image. At least one
accessible image page must remain; fully inaccessible or empty spaces are not
supported by all current backends.

This is a bounded native VM slice, not completion of M1 or M5.
Runtime-sized allocators and copy-on-write fork (the default; see
[NATIVE_COW_FORK](NATIVE_COW_FORK.md)) arrived with `feat/mm-general`. Demand
paging, coherent shared file mappings, file-fault `SIGBUS`, cross-process shared-frame futex keys, unbounded
heap growth, reclaim/OOM policy and SMP shootdown remain separate work.

## Verification

```sh
python3 scripts/test_native_vm.py
python3 scripts/test_native_vm.py --sanitize
python3 scripts/test_native_mapping_pages.py
python3 scripts/test_native_mapping_pages.py --sanitize
```

The hosted test compiles the actual transaction helpers with a fault-injectable
backend. It checks zeroing, protection metadata and filtering, partial unmap,
holes, validation, allocation pressure, table/area capacity and failure rollback.
It does not prove user privilege or hardware isolation. Sanitizer mode enables
ASan/UBSan except unrelated virtual-device vptr instrumentation.

ObfuscationOS `posix-check` supplies the separate hardware gate: direct native
loads/stores, read-only and inaccessible-page faults in children, protection
restoration, private fork isolation, shared anonymous fork visibility,
partial unmap and pressure/reclamation. These
must pass through the exported Kernel SDK and each target's QEMU execution path.
The separate OS musl probe (`--require-heap`) executes mapped `brk`, fixed zero
replacement, malloc/calloc/realloc, alignment, repeated reuse and exhaustion
recovery. Its application is compiled with `-fno-builtin` so optimization cannot
remove the pressure loop or its libc calls. This flag does not rebuild libc.
