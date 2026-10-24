# Bounded physical-page-backed object pool

Namespace `ok::memory`:

- `ObjectHandle { const void *pool; u32 page_slot; u32 object_slot; u64 generation; }`, equality.
- `ObjectAccounting { usize live_objects, backing_pages, quarantined_pages, empty_pages, peak_objects; u64 allocations, destructions; }`.
- `template<class T, ObjectPageProvider P, usize MaximumPages> class ObjectPool`.
- `static constexpr usize slots_per_page = P::page_size / sizeof(T)`; `max_pages = MaximumPages`; `capacity = MaximumPages * slots_per_page`.
- `explicit ObjectPool(P &provider, usize max_objects = capacity)`; invalid zero/overcapacity limit causes create to return invalid_argument before any provider call.
- `template<class... Args> Result<ObjectHandle> create(Args&&...)` with nothrow construction constraint; `Result<T*> resolve(ObjectHandle)` plus const overload; `Status destroy(ObjectHandle)`; `Status trim_empty_pages()`; `ObjectAccounting accounting() const`.
- Provider contract: `P::page_size` constant; `P::Lease { std::span<std::byte> bytes; opaque exact token }`; `Result<Lease> allocate_kernel_page()` and `Status release_kernel_page(Lease)`.

Pool is noncopyable/nonmovable, serialized boot-CPU only. Slots reside in provider pages, metadata in pool. Destroy clears exact object once but does not implicitly release slab; trim returns empty pages. Release failure retains/quarantines the exact lease and never retries it; future create fails until reboot/explicit future recovery, while valid existing objects can resolve/destroy. A generation is pool-wide and never wraps. Pool/provider lifetime must exceed every handle and retained lease.


## Ownership and refusal semantics

The caller supplies one stable page provider whose lease storage is supervisor
mapped and inaccessible to user page allocation. Lease values are non-owning opaque tokens; copying or destroying a token must
not release a page. Only the provider release operation retires ownership. The
pool owns no mapping capability: it retains exact leases and uses only their supplied byte spans.
Construction requires nothrow C++ placement construction, nothrow destruction,
and a type no larger or more aligned than one provider page. No global heap,
exception or virtual dispatch is used. Each free/reused object region is zeroed;
a newly acquired full page is zeroed before any object publication.

The generation sequence is pool-wide and is never reset when a slab is trimmed.
A handle names the exact pool, page metadata index, object slot and generation.
Resolve yields a borrow only for the caller's synchronized operation. Handles
are Kernel-internal values, not a user ABI. Do not destroy/reconstruct the pool
at the same address while old handles survive. No automatic destructor releases
fallible resources: drain external users, destroy live handles, trim, inspect
status/accounting, and retain the pool/provider if any quarantined lease remains.

After provider allocation failure, no object or page metadata is published.
Malformed spans are never dereferenced; their exact lease is released when it
cannot alias retained storage. Suspected alias leases are retained without
release because release could invalidate a live object's mapping. A release
failure keeps the exact lease quarantined and prevents future create operations;
resolve and destroy of existing valid objects remain available. Trim never
retries quarantined leases and may release other healthy empty slabs. Failed
allocations, stale handles and duplicate destroy never run object destructors.
The provider must not retire/reuse a page if release reports failure.

Mutation methods reject reentrancy, including constructors/destructors or
provider callbacks entering the same pool. This guard is not a concurrency lock.
Allocation is boot-task context only, with no IRQ or secondary-CPU caller and
no implicit reclaim/sleep. The live object bound and compile-time page bound are
separate policy limits. The first runtime consumer is owned by the native
runtime integration; this header by itself does not prove any user execution.

## Verification scopes

`scripts/test_object_pool.py --output <new-directory>` builds and runs hosted
fault/ownership tests with xmake, retaining exact sources, commands and hashes.
Add `--sanitize` for ASan/UBSan, with LeakSanitizer explicitly disabled.
The hosted provider supplies aligned real byte storage but models page mapping
and allocation authority. All-ten freestanding builds, actual native runtime
object usage and supervisor mapping protection remain separate gates.
