# Native user and Kernel object page ownership

`NativePagePool` remains the sole owner of retained `PhysicalFrame` handles for
the serialized boot-CPU native runtime. It now distinguishes user backing from
Kernel object backing within the same allocator and registry. This is a bounded
bootstrap provider, not a general heap, concurrent allocator or completed M1.

## Provider interface and purpose isolation

`activate(frames, source, kernel_reserve_pages)` requires an unused allocator,
4096-byte pages, at most 1024 pages, a declared linker or qualified firmware
source, and a reserve no larger than the total. Zero reserve preserves the old
user-only contract. Activation does not establish page tables: the caller must
already have initialized the architecture backend and established supervisor
access to all supplied ranges. The pool and allocator outlive every lease and
must not be reset, copied or moved while ownership survives.

`allocate()` returns a user backing address. Its quota is exactly
`total_count() - kernel_reserve_count()`, even when no objects exist. This leaves
the configured reserve available when user allocation reaches its quota.
`allocate_kernel_page()` returns `Result<NativePagePool::Lease>` and is capped by
the object reserve. A lease exposes an aligned, complete 4096-byte supervisor
`std::span<std::byte>` named `bytes`; its pool and physical ownership token remain
private. The typed object pool initializes bytes and manages C++ lifetimes.

`release_kernel_page(lease)` checks exact pool identity, allocator identity,
index, generation, original address, purpose and unmodified byte span before
returning its frame. Foreign, default, modified or stale leases fail without
changing ownership. A failed underlying release leaves the registry and counters
unchanged. The caller retains that lease for its explicit quarantine policy.
User `owns`, `handle`, `release`, `pin` and `unpin` reject object pages. No public
object operation returns a `PhysicalFrame` that a user mapping can adopt. Runtime
must call `owns` for every proposed user mapping, including retained PROT_NONE
pages, before passing any mapping to an architecture backend.

## Supervisor address contract

The provider uses the supplied frame address verbatim; it does not reinterpret
low physical bus addresses as universally dereferenceable pointers. Firmware
frames have already been bounded by the architecture's qualified supervisor
aperture. Linker fallback frames retain their established Kernel mapping.

In MIPS/MIPS64 fallback builds, the linker allocation address is the existing
KSEG0/KSEG1 supervisor alias (sign-extended on N64). The native backend checks that
segment, dereferences the alias for copies, and masks the low 29 address bits
only when producing EntryLo. `Lease.bytes` therefore retains every alias bit.
Masking it to a physical address here would violate the backend's mapping
contract. Hosted address-model tests verify preservation without dereferencing
these synthetic aliases. Such checks are not hardware isolation evidence.

## Accounting and lifetime

`used_count()` counts all registry-owned pages and equals `user_count()` plus
`object_count()`. Under the exclusive-owner contract, adding `free_count()` gives
`total_count()`. The user quota and object reserve are separately observable.
`allocations()`, `releases()` and `peak_count()` cover both page purposes.

`pinned_count()` counts user pages with one or more surviving mapping-retirement
pins; `pending_count()` counts their requested-but-deferred releases. These are
subsets of user pages, not extra owned frames. Repeated pins consume one page,
and a pending release returns its frame only after the final pin retires.
Object pages never enter this user mapping quarantine. ObjectPool owns its
separate retained-lease quarantine when page return fails. After an ordinary
object return the physical frame becomes free, but its quota remains reserved
from user allocation. Pages have no fixed physical partition by purpose.

## Evidence boundaries

The existing hosted registry/generation/quarantine test is retained and extended
with aligned backing, reserve exhaustion, independent user/object accounting,
foreign/modified/default/stale leases, generation reuse, alias preservation and
null-backing refund. The runner uses xmake and accepts `--output` for retained
source/tool/command/binary hashes; `--sanitize` adds ASan+UBSan with leak detection
disabled. Without `--output`, it preserves the previous temporary-run usage.

Freestanding target compilation checks all ten architecture profiles separately.
Object construction/import checks belong to ObjectPool. Runtime consumer tests,
actual privileged page writes, native/musl guests and supervisor-only PTE/fault
checks remain distinct gates. No SDK export, physical-platform, IRQ-safety or SMP
claim follows from these provider tests.
