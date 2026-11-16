// Native page pool, address-space retirement, mmap/mprotect/munmap/brk and fork copies.
#include "private.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
#ifdef OK_KERNEL_FREESTANDING
namespace
{
constexpr bool linker_pool_required = false; // Frames come from the kernel allocator over discovered RAM.
} // namespace
#else
namespace
{
constexpr bool linker_pool_required = true; // Hosted backing, never firmware evidence.
} // namespace
#endif
namespace
{
// Hosted models only: a host-backed arena with its own allocator.
constexpr usize hosted_page_count = 1024;
alignas(4096) std::array<std::array<std::byte, hw::page_size>, linker_pool_required ? hosted_page_count : 0> physical_pages;
memory::FrameAllocator native_frames;
memory::FrameMetadataStorage<linker_pool_required ? hosted_page_count : 1> native_frame_metadata;
} // namespace
NativePagePool page_pool;
SharedMemoryFiles shared_files;
#ifdef OK_KERNEL_FREESTANDING
namespace
{
Status memory_preparation = Status::not_initialized("native memory not prepared at entry");
bool memory_preparation_attempted{};
} // namespace
#endif
std::array<QuarantinedSpace, sched::max_processes + 1> quarantined_spaces{};
constinit smp::FineLock<smp::LockRank::mm_retire> mm_retire_lock{};
constinit std::atomic<u32> quarantine_live{};
namespace
{
using RetireGuard = decltype(mm_retire_lock)::Guard;
void count_live(bool add)
{
    const auto live = quarantine_live.load(std::memory_order_relaxed);
    quarantine_live.store(add ? live + 1 : live - 1, std::memory_order_relaxed);
}
} // namespace
QuarantinedSpace *reserve_quarantine()
{
    const RetireGuard guard(mm_retire_lock);
    for (auto &saved : quarantined_spaces)
        if (!saved.used && !saved.reserved && !saved.busy)
        {
            saved.reserved = true;
            return &saved;
        }
    return nullptr;
}
void cancel_quarantine(QuarantinedSpace &saved)
{
    const RetireGuard guard(mm_retire_lock);
    saved.reserved = false;
}
void publish_quarantine(QuarantinedSpace &saved, const hw::AddressSpace &space, bool table_retired, bool pin_complete)
{
    const RetireGuard guard(mm_retire_lock);
    if (!saved.used) count_live(true);
    saved.space = space;
    saved.used = true;
    saved.reserved = false;
    saved.busy = true;
    saved.table_retired = table_retired;
    saved.pin_complete = pin_complete;
}
QuarantinedSpace *claim_quarantine(const hw::AddressSpace &space, bool retired_only)
{
    const RetireGuard guard(mm_retire_lock);
    for (auto &saved : quarantined_spaces)
        if (saved.used && !saved.busy && (!retired_only || saved.table_retired) &&
            (space.root == 0 || same_space(saved.space, space)))
        {
            saved.busy = true;
            return &saved;
        }
    return nullptr;
}
void unclaim_quarantine(QuarantinedSpace &saved)
{
    const RetireGuard guard(mm_retire_lock);
    saved.busy = false;
}
void set_quarantine_state(QuarantinedSpace &saved, bool table_retired, bool pin_complete)
{
    const RetireGuard guard(mm_retire_lock);
    saved.table_retired = table_retired;
    saved.pin_complete = pin_complete;
}
bool space_quarantined(const hw::AddressSpace &space)
{
    const RetireGuard guard(mm_retire_lock);
    for (const auto &saved : quarantined_spaces)
        if (saved.used && saved.space.root && same_space(saved.space, space)) return true;
    return false;
}
namespace
{
constexpr usize kernel_object_pages = poll_object_pages + futex_object_pages + pipe_read_object_pages + pipe_write_object_pages + pty_wait_object_pages + child_wait_object_pages + native_udp_receive_pages + linux_udp_receive_pages;
} // namespace
static_assert(kernel_object_pages < hosted_page_count);
namespace
{
// Translation tables and mapping metadata grow with the processes they serve:
// beside the runtime objects, the reserve holds a sixteenth of the frames free
// at activation, at least 256. User pages never take reserved frames.
constexpr usize object_reserve_pages(usize free) { return kernel_object_pages + (free / 16 > 256 ? free / 16 : 256); }
constexpr usize kernel_reserve_pages = object_reserve_pages(hosted_page_count);
} // namespace
static_assert(kernel_reserve_pages < hosted_page_count);
void emit_pool_stats()
{
    emit("\nOK_NATIVE_FRAMES version=3 source=");
    emit(page_pool.source() == NativePagePool::Source::qualified_firmware ? "firmware" : "linker");
    emit(" used="); emit_number(page_pool.used_count());
    emit(" total="); emit_number(page_pool.total_count());
    emit(" allocations="); emit_number(page_pool.allocations());
    emit(" releases="); emit_number(page_pool.releases());
    emit(" pinned="); emit_number(page_pool.pinned_count());
    emit(" user="); emit_number(page_pool.user_count());
    emit(" objects="); emit_number(page_pool.object_count());
    emit(" free="); emit_number(page_pool.free_count());
    emit(" pending="); emit_number(page_pool.pending_count());
    emit(" reserve="); emit_number(page_pool.kernel_reserve_count());
    emit(" user_limit="); emit_number(page_pool.user_capacity_count());
    emit(" object_quarantine="); emit_number(page_pool.quarantined_kernel_page_count()); emit("\n");
    const auto files = shared_files.ledger();
    emit("OK_NATIVE_SHARED_FILES_LEDGER version=1 files="); emit_number(files.files);
    emit(" aliases="); emit_number(files.aliases);
    emit(" retiring_files="); emit_number(files.retiring_files);
    emit(" retained_frames="); emit_number(files.retained_frames);
    emit(" reserved_cells="); emit_number(files.reserved_cells); emit("\n");
    if (copy_on_write_fork) { emit("OK_NATIVE_COW version=1 shared="); emit_number(copy_on_write_stats.shared);
        emit(" copied="); emit_number(copy_on_write_stats.copied); emit(" reused="); emit_number(copy_on_write_stats.reused); emit("\n"); }
}
Status activate_pool([[maybe_unused]] Kernel *kernel)
{
    if (page_pool.source() != NativePagePool::Source::inactive)
        return Status::success();
#ifdef OK_KERNEL_FREESTANDING
    if (!memory_preparation) return memory_preparation;
    if constexpr (!linker_pool_required)
    {
        if (!kernel) kernel = runtime_kernel;
        if (!kernel) return Status::not_initialized("native page pool requires its kernel allocator");
        // Shared with every other kernel owner: user pages may take what is
        // free at activation outside the kernel-object reserve.
        auto &frames = kernel->memory().frames();
        const auto reserve = object_reserve_pages(frames.free_frames());
        if (frames.free_frames() <= reserve)
            return Status::no_memory("discovered RAM cannot hold the native kernel-object reserve");
        return page_pool.activate(frames, NativePagePool::Source::qualified_firmware, reserve,
                                  frames.free_frames() - reserve);
    }
#endif
    const memory::MemoryRegion region{reinterpret_cast<uptr>(physical_pages.data()),sizeof(physical_pages),
                                      memory::RegionType::usable};
    if (auto status = native_frames.initialize({&region,1},hw::page_size,native_frame_metadata.bytes); !status) return status;
    return page_pool.activate(native_frames, NativePagePool::Source::linker_reserved, kernel_reserve_pages);
}
#ifdef OK_ENABLE_TEST_POINTS
Status check_runtime_objects()
{
    std::array<memory::ObjectHandle, sched::max_processes> handles{};
    usize count{};
    Status result{};
    const auto users_before = page_pool.user_count();
    for (; count < handles.size(); ++count)
    {
        auto created = poll_objects.create();
        if (!created) { result = created.status(); break; }
        handles[count] = created.value();
        auto resolved = poll_objects.resolve(handles[count]);
        if (!resolved) { result = resolved.status(); ++count; break; }
        auto &record = *resolved.value();
        const auto backing = reinterpret_cast<uptr>(&record) & ~uptr{hw::page_size - 1};
        if (record.pid || record.count || page_pool.owns(backing) || page_pool.handle(backing))
        { result = Status::fault("kernel object initialization or purpose failed"); ++count; break; }
        record.pid = static_cast<sched::ProcessId>(count + 1);
        record.fds.back().fd = static_cast<i32>(count + 117);
    }
    if (result)
    {
        auto excess = poll_objects.create();
        if (excess)
        {
            static_cast<void>(poll_objects.destroy(excess.value()));
            result = Status::fault("kernel object limit was not enforced");
        }
        else if (excess.status().code() != StatusCode::no_memory)
            result = excess.status();
        if (page_pool.object_count() != poll_object_pages || page_pool.user_count() != users_before)
            result = Status::fault("kernel object page accounting failed");
        for (usize i = 0; i < count; ++i)
        {
            auto resolved = poll_objects.resolve(handles[i]);
            if (!resolved || resolved.value()->pid != i + 1 ||
                resolved.value()->fds.back().fd != static_cast<i32>(i + 117))
                result = Status::fault("kernel object writes did not survive allocation");
        }
    }
    for (usize i = 0; i < count; ++i)
    {
        if (auto status = poll_objects.destroy(handles[i]); !status && result) result = status;
        if (poll_objects.resolve(handles[i])) result = Status::fault("destroyed kernel object remains resolvable");
    }
    if (auto status = poll_objects.trim_empty_pages(); !status && result) result = status;
    if (page_pool.object_count()) result = Status::fault("kernel object test retained backing");
    emit("OK_NATIVE_OBJECTS version=1 scope=kernel status="); emit(result ? "pass" : "fail");
    emit(" records="); emit_number(count); emit(" pages="); emit_number(poll_object_pages);
    emit(" slots_per_page="); emit_number(PollObjects::slots_per_page);
    emit(" remaining="); emit_number(page_pool.object_count()); emit("\n");
    return result;
}
#endif
bool same_space(const hw::AddressSpace &a, const hw::AddressSpace &b)
{
    return a.root == b.root && a.slot == b.slot && a.generation == b.generation;
}
namespace
{
// The process whose live space `space` is. The kernel lock holder finds it
// among the processes; the per-CPU caller binding replaces the scan once user
// accesses run without that lock.
sched::ProcessControlBlock *space_owner(const hw::AddressSpace &space)
{
    if (!runtime_kernel || !space.root) return nullptr;
    for (auto &process : runtime_kernel->scheduler().processes())
        if (same_space(sched::read_native_space(process.resources()), space)) return &process;
    return nullptr;
}
// Holding the owner's mm: makes every withheld page of the range writable.
// Returns how many it resolved.
Result<usize> resolve_withheld([[maybe_unused]] sched::ProcessResources &resources,
                               [[maybe_unused]] const hw::AddressSpace &space, [[maybe_unused]] uptr address,
                               [[maybe_unused]] usize size)
{
    usize resolved{};
#ifdef OK_NATIVE_COW_FORK
    smp::check_held(resources.mm, "copy-on-write resolution");
    if (resources.mm.dead || !size || address > ~uptr{0} - (size - 1)) return resolved;
    const uptr last = address + size - 1;
    for (uptr page = address & ~(hw::page_size - 1);; page += hw::page_size)
    {
        auto changed = resolve_copy_on_write(resources.native_pages, space, page, page_pool);
        if (!changed) return changed.status();
        resolved += changed.value();
        if (last - page < hw::page_size) break;
    }
#endif
    return resolved;
}
} // namespace
Status resolve_user_access(const hw::AddressSpace &space, uptr address, usize size, bool write,
                           const uaccess::Attempt *attempt)
{
    auto *owner = space_owner(space);
    if (!owner) return attempt ? (*attempt)() : Status::not_found("user space has no live owner");
    auto &resources = owner->resources();
    // A write into a process waits for that process's sections, which walk
    // the records rewritten here; the drain runs before mm is taken.
    if constexpr (smp_sections)
        if (write) drain_if_pinned(owner->pid());
    const sched::AddressSpaceGate::Hold hold(resources.mm);
    if (!same_space(resources.native_space, space))
        return attempt ? (*attempt)() : Status::not_found("user space was replaced");
    usize resolved{};
    if (write)
    {
        auto changed = resolve_withheld(resources, space, address, size);
        if (!changed)
        {
            if (changed.status().code() != StatusCode::no_memory) runtime_failure = changed.status();
            return attempt ? Status::fault("copy-on-write resolution failed") : changed.status();
        }
        resolved = changed.value();
    }
    if (attempt) return (*attempt)();
    return resolved ? Status{} : Status::not_found("no withheld page in the user range");
}
bool resolve_user_fault(sched::ProcessResources &resources, const hw::Trap &trap)
{
    const hw::AddressSpace space = resources.native_space;
    if constexpr (smp_sections)
        if (runtime_kernel) drain_if_pinned(runtime_kernel->scheduler().resources_owner(resources));
    const sched::AddressSpaceGate::Hold hold(resources.mm);
    if (resources.mm.dead) return false;
    // A write to a withheld page: copy it or regain write access.
    auto resolved = resolve_withheld(resources, space, trap.fault_address, 1);
    if (!resolved)
    {
        if (resolved.status().code() != StatusCode::no_memory) runtime_failure = resolved.status();
        return false;
    }
    if (resolved.value()) return true;
    // A fault on a translation another CPU replaced meanwhile (break before
    // make): the page is mapped for any data access now, so it repeats.
    return trap.translation_changed && hw::user_range(space, trap.fault_address, 1, true);
}
void release_address_space(sched::ProcessResources &resources)
{
    const sched::AddressSpaceGate::Hold hold(resources.mm);
    auto space = resources.native_space;
    release(space, resources.native_pages);
    sched::publish_native_space(resources, space);
    resources.mm.dead = true;
}
namespace
{
Status release_page_reference(const memory::PhysicalFrame &frame, bool &consumed)
{
    consumed = false;
    if (shared_files.owns(frame)) return shared_files.release_alias(frame, consumed);
    const auto status = page_pool.release_reference(frame);
    consumed = status.ok();
    return status;
}
Status release_page_reference(const memory::PhysicalFrame &frame)
{
    bool consumed{};
    return release_page_reference(frame, consumed);
}
Status retain_page_reference(const memory::PhysicalFrame &frame)
{
    return shared_files.owns(frame) ? shared_files.retain_alias(frame) : page_pool.retain_shared(frame);
}
const memory::PhysicalFrame *saved_frame(const Pages &pages, const hw::PageMapping &page)
{
    const auto *frame = pages.frame(page);
    return frame && frame->address == page.physical_address && page_pool.valid(*frame) ? frame : nullptr;
}
} // namespace
const memory::PhysicalFrame *owned_frame(const Pages &pages, const hw::PageMapping &page)
{
    const auto *frame = saved_frame(pages, page);
    return frame && page_pool.reference_count(frame->address) ? frame : nullptr;
}
Status discharge_quarantine(QuarantinedSpace &saved, bool all, uptr begin, usize size)
{
    using Ownership = QuarantinedSpace::Ownership;
    Status result{};
    for (usize index = 0; index < saved.pages.size(); ++index)
    {
        const auto &page = saved.pages[index];
        const bool selected = all || (size && page.virtual_address >= begin && page.virtual_address - begin < size);
        if (selected && saved.ownership(index) == Ownership::borrowed) saved.ownership(index) = Ownership::pending;
    }
    if (!saved.pin_complete && !saved.table_retired)
        return Status::busy("quarantine cannot discharge an incompletely pinned native table");
    for (usize index = 0; index < saved.pages.size(); ++index)
    {
        const auto &page = saved.pages[index];
        auto ownership = saved.ownership(index);
        if (ownership != Ownership::pending) continue;
        const auto *frame = owned_frame(saved.pages, page);
        if (!frame)
        {
            if (result) result = Status::fault("quarantine lost exact owner discharge authority");
            continue;
        }
        bool consumed{};
        const auto status = release_page_reference(*frame, consumed);
        // Alias discharge can succeed before a fallible canonical-file
        // retirement. Such a failure must never retry the consumed alias.
        if (consumed) ownership = Ownership::discharged;
        if (!status && result) result = status;
    }
    return result;
}
Status finish_quarantine(QuarantinedSpace &saved, bool retry_owners)
{
    if (!saved.table_retired) return Status::busy("quarantined native table still borrows mapping metadata");
    Status result = retry_owners ? discharge_quarantine(saved) : Status{};
    for (usize index = 0; index < saved.pages.size(); ++index)
    {
        if (saved.ownership(index) == QuarantinedSpace::Ownership::pending) continue;
        if (!saved.pin_held(index)) continue;
        const auto *frame = saved_frame(saved.pages, saved.pages[index]);
        if (!frame)
        {
            if (result) result = Status::fault("quarantine lost exact pin retirement authority");
            continue;
        }
        const auto status = page_pool.unpin(*frame);
        if (status)
        {
            saved.pin_held(index) = false;
            --saved.pinned_count;
        }
        else if (result) result = status;
    }
    for (usize index = 0; index < saved.pages.size(); ++index)
        if (saved.ownership(index) == QuarantinedSpace::Ownership::pending || saved.pin_held(index))
            return result ? Status::busy("quarantine still owns failed frame retirement") : result;
    if (auto status = saved.pages.clear(); !status) return status;
    // The emptied slot goes back free once its claimant unclaims it.
    const RetireGuard guard(mm_retire_lock);
    if (saved.used) count_live(false);
    saved.space = {};
    saved.used = false;
    saved.reserved = false;
    saved.table_retired = false;
    saved.pin_complete = false;
    return result;
}
Status retry_quarantined_retirement()
{
    Status result{};
    if (quarantine_live.load(std::memory_order_relaxed) == 0) return result;
    // Pop one retired slot at a time under the lock; finish it outside.
    std::array<QuarantinedSpace *, std::tuple_size_v<decltype(quarantined_spaces)>> claimed{};
    usize count{};
    while (count < claimed.size())
    {
        auto *saved = claim_quarantine({}, true);
        if (!saved) break;
        claimed[count++] = saved;
        if (auto status = finish_quarantine(*saved); !status && result) result = status;
    }
    for (usize index = 0; index < count; ++index) unclaim_quarantine(*claimed[index]);
    if (!result) runtime_failure = result;
    return result;
}
// A range transaction reserves retry metadata before acquiring data ownership
// or publishing replacement tables. Unselected records remain borrowed.
class PageRetirementReservation final : private NonCopyable
{
  public:
    PageRetirementReservation(const PageRetirementReservation &) = delete;
    PageRetirementReservation &operator=(const PageRetirementReservation &) = delete;
    PageRetirementReservation &operator=(PageRetirementReservation &&) = delete;
    PageRetirementReservation(PageRetirementReservation &&other) noexcept
        : saved_(std::exchange(other.saved_, nullptr)) {}
    ~PageRetirementReservation() { if (saved_) cancel_quarantine(*saved_); }
    [[nodiscard]] static Result<PageRetirementReservation> reserve()
    {
        if (auto *saved = reserve_quarantine()) return PageRetirementReservation{*saved};
        return Status::no_memory("native range retirement bookkeeping is full");
    }
    Status release_range(Pages &pages, uptr begin, usize size)
    {
        if (!saved_ || !saved_->reserved || saved_->used)
            return Status::fault("native range retirement reservation lost its owner");
        auto &saved = *saved_;
        saved.pages = std::move(pages);
        saved.pinned_count = 0;
        saved.reset(QuarantinedSpace::Ownership::borrowed);
        publish_quarantine(saved, {}, true, true);
        saved_ = nullptr;
        const auto discharged = discharge_quarantine(saved, false, begin, size);
        const auto retired = finish_quarantine(saved, false);
        unclaim_quarantine(saved);
        if (!discharged) return discharged;
        return retired;
    }
  private:
    explicit PageRetirementReservation(QuarantinedSpace &saved) : saved_(&saved) {}
    QuarantinedSpace *saved_{};
};
namespace
{
Status quarantine(const hw::AddressSpace &space, Pages &pages)
{
    if (space_quarantined(space))
    {
        if (pages.empty()) return Status::success();
        runtime_failure = Status::already_exists("native space is already quarantined");
        __builtin_trap();
    }
    if (auto *reserved = reserve_quarantine())
    {
        auto &saved = *reserved;
        saved.pages = std::move(pages);
        saved.reset(QuarantinedSpace::Ownership::borrowed);
        saved.pinned_count = 0;
        publish_quarantine(saved, space, false, false);
        Status status{};
        for (usize index = 0; index < saved.pages.size() && status; ++index)
        {
            const auto *frame = owned_frame(saved.pages, saved.pages[index]);
            if (!frame) status = runtime_failure = Status::fault("quarantine lost its exact mapping frame");
            else if (status = page_pool.pin(*frame); !status)
                // Retain the metadata and every ownership edge. No alias may
                // be discharged while this borrow is incompletely pinned.
                runtime_failure = status;
            else
            {
                saved.pin_held(index) = true;
                ++saved.pinned_count;
            }
        }
        if (status) set_quarantine_state(saved, false, true);
        unclaim_quarantine(saved);
        return status;
    }
    // Returning here would destroy the caller's metadata while an unretired
    // backend may still borrow its view. This is a backend slot-contract breach.
    runtime_failure = Status::overflow("native retirement quarantine exhausted");
    __builtin_trap();
}
Status forget_quarantine(const hw::AddressSpace &space)
{
    if (!space.root) return {};
    auto *saved = claim_quarantine(space, false);
    if (!saved) return {};
    set_quarantine_state(*saved, true, saved->pin_complete);
    const auto status = finish_quarantine(*saved);
    unclaim_quarantine(*saved);
    if (!status) runtime_failure = status;
    return status;
}
// Discharges the staged frames of a quarantined space that `pinned` admits.
void discharge_staged(const hw::AddressSpace &space, bool pinned, uptr begin, usize size, bool record)
{
    auto *saved = space.root ? claim_quarantine(space, false) : nullptr;
    if (!saved) return;
    if (!pinned || saved->pin_complete)
        if (auto released = discharge_quarantine(*saved, false, begin, size); !released && record)
            runtime_failure = released;
    unclaim_quarantine(*saved);
}
Status retire_space(hw::AddressSpace &space, Pages &pages, uptr staged_begin = 0, usize staged_size = 0)
{
    const auto previous = space;
    if (previous.root)
        if (auto *saved = claim_quarantine(previous, true))
        {
            unclaim_quarantine(*saved);
            space = {};
            return forget_quarantine(previous);
        }
    if (auto status = hw::destroy_space(space); !status)
    {
        if (auto retained = quarantine(previous, pages); !retained)
        {
            if (staged_size) discharge_staged(previous, false, staged_begin, staged_size, false);
            runtime_failure = retained;
            return retained;
        }
        // mmap owns freshly allocated backing only in the replacement. Once
        // the failed table is pinned, mark those frames pending so successful
        // later retirement returns them to the allocator.
        if (staged_size) discharge_staged(previous, true, staged_begin, staged_size, true);
        runtime_failure = status;
        return status;
    }
    return forget_quarantine(previous);
}
} // namespace
void release(hw::AddressSpace &space, Pages &pages)
{
    for (const auto &page : pages)
        if (!owned_frame(pages, page))
        { runtime_failure = Status::fault("native teardown lost exact mapping frame authority"); return; }
    if (space.root != 0 && !retire_space(space,pages))
    {
        // A failed final retirement transferred the owner into quarantine.
        // Every user frame becomes pending while its borrowed view is pinned.
        if (pages.empty())
            if (auto *saved = claim_quarantine(space, false))
            {
                if (auto status = discharge_quarantine(*saved, true); !status) runtime_failure = status;
                unclaim_quarantine(*saved);
            }
        return;
    }
    if (pages.empty())
    {
        if (auto status = pages.clear(); !status) runtime_failure = status;
        return;
    }
    // Reserve bounded retry bookkeeping before consuming any ownership edge,
    // including when no hardware table was ever created for these staged pages.
    if (auto *reserved = reserve_quarantine())
    {
        auto &saved = *reserved;
        saved.pages = std::move(pages);
        saved.pinned_count = 0;
        saved.reset(QuarantinedSpace::Ownership::borrowed);
        publish_quarantine(saved, space, true, true);
        const auto discharged = discharge_quarantine(saved, true);
        const auto retired = finish_quarantine(saved, false);
        unclaim_quarantine(saved);
        if (!discharged) runtime_failure = discharged;
        else if (!retired) runtime_failure = retired;
        return;
    }
    runtime_failure = Status::no_memory("native frame retirement bookkeeping is full");
}
Status allocate(Pages &pages, uptr address, u8 flags, bool shared)
{
    for (const auto &page : pages)
        if (page.virtual_address == address)
            return Status::invalid_argument("ELF pages overlap");
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return status;
#endif
    if (auto status = pages.reserve(page_pool, pages.size() + 1, true); !status) return status;
    auto frame = page_pool.allocate_frame();
    if (!frame) return frame.status();
    auto *bytes = reinterpret_cast<std::byte *>(frame.value().address);
    for (usize i=0;i<hw::page_size;++i) bytes[i] = std::byte{};
    if (shared)
    {
        if (auto status = page_pool.mark_shared(frame.value()); !status)
        {
            static_cast<void>(page_pool.release_reference(frame.value()));
            return status;
        }
    }
    auto status = pages.push_back({address,frame.value().address,flags}, frame.value());
    if (!status) static_cast<void>(page_pool.release_reference(frame.value()));
    return status;
}
Result<hw::AddressSpace> create_page_space(const Pages &pages)
{
    if (!pages.exact_frames())
        return Status::fault("native address space requires exact mapping frame tokens");
    bool accessible = false;
    for (const auto &page : pages)
    {
        if (!owned_frame(pages, page))
            return Status::fault("user mapping requires a retained user-purpose frame");
        accessible |= page.flags != 0;
    }
    if (!accessible)
        return Status::no_memory("native address space requires accessible image pages");
    return hw::create_space(pages.view());
}
namespace
{

Result<usize> native_page_length(usize length)
{
    if (length == 0 || length > ~usize{0} - (hw::page_size - 1))
        return Status::invalid_argument("invalid native VM length");
    return (length + hw::page_size - 1) & ~(hw::page_size - 1);
}

Result<u8> native_page_flags(uptr protection, bool write_implies_read = false)
{
    // Every current architecture backend requires read permission on a
    // writable PTE. The sealed-file adapter admits PROT_WRITE with that
    // existing read/write representation, rather than promising write-only PTEs.
    if (write_implies_read && protection == posix::prot_WRITE)
        return static_cast<u8>(posix::prot_READ | posix::prot_WRITE);
    if (protection != posix::prot_NONE && protection != posix::prot_READ &&
        protection != (posix::prot_READ | posix::prot_WRITE))
        return Status::invalid_argument("native anonymous VM supports none, read or read/write protection");
    return static_cast<u8>(protection);
}

bool valid_native_range(uptr address, usize length)
{
    return address >= hw::user_begin && address < hw::user_end &&
           (address & (hw::page_size - 1)) == 0 && length <= hw::user_end - address;
}

bool private_signal_reserved_range(const sched::ProcessResources &resources, uptr address, usize length)
{
#ifdef OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE
    // The caller already checked this complete interval against the user window.
    return resources.personality == Personality::linux && length &&
        address < private_signal_code_address + hw::page_size &&
        (address >= private_signal_code_address || length > private_signal_code_address - address);
#else
    static_cast<void>(resources); static_cast<void>(address); static_cast<void>(length);
    return false;
#endif
}

bool owns_pool_page(const Pages &pages, const hw::PageMapping &page)
{
    return owned_frame(pages, page) != nullptr;
}
} // namespace
bool shared_pool_page(const Pages &pages, const hw::PageMapping &page)
{
    return owned_frame(pages, page) && page_pool.is_shared(page.physical_address);
}
namespace
{
Status page_memory_map(const Pages &pages, sched::MemoryMap &map)
{
    for (usize first = 0; first < pages.size();)
    {
        usize last = first + 1;
        while (last < pages.size() && intended_access(pages[last]) == intended_access(pages[first]) &&
               shared_pool_page(pages, pages[last]) == shared_pool_page(pages, pages[first]) &&
               pages[last].virtual_address == pages[last - 1].virtual_address + hw::page_size)
            ++last;
        if (auto status = map.add_area({pages[first].virtual_address, (last - first) * hw::page_size,
                                      memory::page_user | intended_access(pages[first]) |
                                          (shared_pool_page(pages, pages[first]) ? static_cast<usize>(memory::page_shared) : usize{0})}); !status.ok())
            return Status::no_memory("native VM area capacity exceeded");
        first = last;
    }
    return Status::success();
}

// One mmap's frames, staged before mm (stage then build): fresh zeroed frames
// (marked shared for MAP_SHARED) or aliases of a sealed shared file's pages,
// with a private file's bytes read in. Records are keyed by their offset in
// the range; transfer() moves them into the candidate at their final address.
// Whatever was not moved goes back when this goes, after mm was released.
class StagedRange final : private NonCopyable
{
  public:
    StagedRange() = default;
    ~StagedRange()
    {
        for (usize index = 0; index < pages_.size(); ++index)
            if (!pages_.retirement(index))
                if (const auto *frame = pages_.frame(index))
                    if (auto status = release_page_reference(*frame); !status) runtime_failure = status;
        if (auto status = pages_.clear(); !status) runtime_failure = status;
    }
    Status stage(usize size, u8 flags, bool shared, SharedFileHandle shared_file, usize offset, fs::Node *source,
                 usize file_size)
    {
#ifndef OK_KERNEL_FREESTANDING
        if (auto status = activate_pool(); !status) return status;
#endif
        if (auto status = pages_.reserve(page_pool, size / hw::page_size, true); !status) return status;
        for (usize mapping_offset = 0; mapping_offset < size; mapping_offset += hw::page_size)
        {
            Status status{};
            if (shared_file)
            {
                auto frame = shared_files.page(shared_file, (offset + mapping_offset) / hw::page_size);
                if (!frame) status = frame.status();
                else if (auto retained = shared_files.retain_alias(frame.value()); !retained) status = retained;
                else
                {
                    status = pages_.push_back({mapping_offset, frame.value().address, flags}, frame.value());
                    if (!status) static_cast<void>(shared_files.release_alias(frame.value()));
                }
            }
            else status = allocate(pages_, mapping_offset, flags, shared);
            if (!status) return status;
        }
        if (!source) return {};
        const usize remaining = file_size - offset;
        // The requested length reserves complete pages. Bytes between length
        // and the end of its final page still reflect the file up to EOF.
        const usize file_bytes = size < remaining ? size : remaining;
        usize copied = 0;
        while (copied < file_bytes)
        {
            const usize chunk = file_bytes - copied < fs::max_file_data ? file_bytes - copied : fs::max_file_data;
            auto read = source->read(offset + copied, chunk);
            if (!read) return read.status();
            if (!read.value().size || read.value().size > chunk)
                return Status::io_error("private file mapping changed during staging");
            if (auto status = store(pages_, copied, read.value().data.data(), read.value().size); !status)
                return status;
            copied += read.value().size;
        }
        return {};
    }
    // Moves every staged record into `candidate` at `address` plus its offset.
    Status transfer(Pages &candidate, uptr address)
    {
        for (usize index = 0; index < pages_.size(); ++index)
        {
            auto page = pages_[index];
            const auto *frame = pages_.frame(index);
            if (!frame) return Status::fault("staged mapping lost its exact frame token");
            page.virtual_address += address;
            if (auto status = candidate.push_back(page, *frame); !status) return status;
            pages_.retirement(index) = 1;
        }
        return {};
    }

  private:
    Pages pages_;
};

Status replace_native_pages(sched::ProcessResources &resources, Pages &candidate,
                            bool &can_release_staged, uptr staged_begin = 0, usize staged_size = 0)
{
    // Holding the process's mm, with the kernel address space restored. Build
    // everything before retiring the old tables or publishing state.
    smp::check_held(resources.mm, "replace_native_pages");
    sched::MemoryMap memory;
    if (auto status = page_memory_map(candidate, memory); !status.ok())
        return status;
    auto replacement = create_page_space(candidate);
    if (!replacement)
        return replacement.status();
    auto previous = resources.native_space;
    const auto previous_identity = previous;
    auto origin_update = syscall_origins.stage_space(runtime_kernel ? &runtime_kernel->scheduler() : nullptr,
        resources, previous_identity, replacement.value());
    if (!origin_update)
    {
        if (auto rollback = retire_space(replacement.value(), candidate, staged_begin, staged_size); !rollback)
        { can_release_staged = false; return rollback; }
        return origin_update.status();
    }
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (!private_ppc_space_update(resources, previous_identity, replacement.value(), false))
    {
        private_ppc_cancel_transactions();
        runtime_failure = Status::fault("private PPC mapping preflight lost readiness");
        started = false;
        if (auto rollback = retire_space(replacement.value(), candidate, staged_begin, staged_size); !rollback)
        { can_release_staged = false; return rollback; }
        return runtime_failure;
    }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (!private_n64_space_update(resources, previous_identity, replacement.value(), false))
    {
        if (auto rollback = retire_space(replacement.value(), candidate, staged_begin, staged_size); !rollback)
        { can_release_staged = false; return rollback; }
        runtime_failure = Status::fault("private n64 mapping publication preflight failed");
        return runtime_failure;
    }
#endif
    if (auto status = hw::destroy_space(previous); !status.ok())
    {
#ifdef OK_PRIVATE_PPC32_SIGNAL
        private_ppc_cancel_transactions();
        runtime_failure = Status::fault("private PPC mapping retirement failed after staging");
        started = false;
#endif
        if (auto rollback = retire_space(replacement.value(),candidate, staged_begin, staged_size); !rollback.ok())
        {
            // Quarantine staged pages if a backend cannot retire fresh tables.
            can_release_staged = false;
            return rollback;
        }
        return status;
    }
    if (auto status = forget_quarantine(resources.native_space); !status)
    {
        runtime_failure = status;
        can_release_staged = false;
        return status;
    }
    sched::publish_native_space(resources, replacement.value());
    resources.native_pages = std::move(candidate);
    resources.memory = memory;
    if (!syscall_origins.commit_space(origin_update.value(), runtime_kernel ? &runtime_kernel->scheduler() : nullptr,
                                     resources))
    {
        can_release_staged = false;
        runtime_failure = Status::fault("committed mapping replacement lost syscall origin owner");
        started = false;
        return runtime_failure;
    }
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (!private_ppc_space_update(resources, previous_identity, replacement.value(), true))
    {
        private_ppc_cancel_transactions();
        can_release_staged = false;
        runtime_failure = Status::fault("private PPC committed mapping publication failed");
        started = false;
        return runtime_failure;
    }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (!private_n64_space_update(resources, previous_identity, replacement.value(), true))
    {
        can_release_staged = false;
        runtime_failure = Status::fault("private n64 committed mapping publication failed");
        return runtime_failure;
    }
#endif
    return Status::success();
}
} // namespace

Result<uptr> native_mmap(sched::ProcessResources &resources, uptr hint, usize length,
                         uptr protection, uptr flags, uptr fd, uptr offset,
                         fs::Node *source, usize file_size, SharedFileHandle shared_file)
{
    const bool fixed = flags & posix::map_FIXED;
    const bool anonymous = flags & posix::map_ANONYMOUS;
    const bool shared = flags & posix::map_SHARED;
    const bool private_mapping = flags & posix::map_PRIVATE;
    if (shared == private_mapping ||
        (flags & ~uptr(posix::map_FIXED | posix::map_ANONYMOUS | posix::map_PRIVATE | posix::map_SHARED)))
        return Status::invalid_argument("native mmap requires exactly one of MAP_PRIVATE and MAP_SHARED");
    if (shared && !anonymous && !shared_file)
        return Status::unsupported("shared file mappings require a coherent page cache");
    if (anonymous)
    {
        if (static_cast<__INTPTR_TYPE__>(fd) != -1 || offset != 0 || source || shared_file)
            return Status::invalid_argument("anonymous native mapping requires fd -1 and offset zero");
    }
    else
    {
        if (shared_file && private_mapping)
            return Status::unsupported("shared file private snapshots are not admitted");
        if (static_cast<__INTPTR_TYPE__>(fd) < 0 || (!source && !shared_file))
            return Status::bad_descriptor("private file mapping requires a readable regular file");
        if (offset % hw::page_size || offset > ~usize{0} - length)
            return Status::invalid_argument("private file mapping offset is unaligned or wraps");
        if (shared_file && (length % hw::page_size || offset > file_size || length > file_size - offset))
            return Status::unsupported("shared file mapping requires complete pages inside sealed size");
        // Without a file-fault path, a complete page beyond EOF cannot deliver
        // SIGBUS. Refuse it instead of silently exposing anonymous zero pages.
        if (offset >= file_size || (length > hw::page_size &&
            (length - 1) / hw::page_size * hw::page_size >= file_size - offset))
            return Status::unsupported("private file mapping includes a page beyond EOF");
    }
    auto rounded = native_page_length(length);
    if (!rounded)
        return rounded.status();
    auto page_flags = native_page_flags(protection, static_cast<bool>(shared_file));
    if (!page_flags)
        return page_flags.status();
    if (shared_file && (protection & posix::prot_WRITE))
        if (auto status = shared_files.check_write(shared_file, 1); !status) return status;
    const usize size = rounded.value();
    if (fixed && ((hint & (hw::page_size - 1)) || !valid_native_range(hint, size)))
        return Status::invalid_argument("native fixed mapping range is invalid");
    if (size / hw::page_size > Pages::capacity)
        return Status::no_memory("native process page capacity exceeded");
    // Where the range goes and what it replaces, from the records as they are
    // now (the caller holds mm). Run read-only before staging, so a refusal
    // allocates nothing, and again under mm before the build.
    struct Plan
    {
        uptr address{};
        usize removed{};
    };
    auto plan = [&]() -> Result<Plan> {
        if (resources.mm.dead)
            return Status::no_process("native address space was torn down");
        auto available = [&](uptr candidate_address) {
            if (!valid_native_range(candidate_address, size))
                return false;
            for (const auto &page : resources.native_pages)
                if (page.virtual_address >= candidate_address && page.virtual_address - candidate_address < size)
                    return false;
            return true;
        };
        uptr address = hint & ~(hw::page_size - 1);
        if (fixed && private_signal_reserved_range(resources, address, size))
            return Status::denied("private signal return page is reserved");
        if (!fixed && (hint == 0 || !available(address)))
        {
            // Keep ordinary allocations away from the conventional ELF base. A
            // hint remains optional: occupied/invalid hints use the same first fit.
            address = hw::user_begin + 16 * 1024 * 1024;
            while (valid_native_range(address, size))
            {
                uptr next = address;
                for (const auto &page : resources.native_pages)
                    if (page.virtual_address >= address && page.virtual_address - address < size &&
                        page.virtual_address + hw::page_size > next)
                        next = page.virtual_address + hw::page_size;
                if (next == address)
                    break;
                address = next;
            }
            if (!available(address))
                return Status::no_memory("native virtual address window exhausted");
        }
        usize removed = 0;
        for (const auto &page : resources.native_pages)
            if (fixed && page.virtual_address >= address && page.virtual_address - address < size)
            {
                if (!owns_pool_page(resources.native_pages, page))
                    return Status::fault("native fixed mapping has no pool owner");
                ++removed;
            }
        if (size / hw::page_size > Pages::capacity - (resources.native_pages.size() - removed))
            return Status::no_memory("native process page capacity exceeded");
        if (auto status = resources.memory_limits.admit(resources.native_pages.size() - removed +
                                                        size / hw::page_size, hw::page_size); !status)
            return status;
        return Plan{address, removed};
    };
    {
        const sched::AddressSpaceGate::Hold hold(resources.mm);
        if (auto planned = plan(); !planned) return planned.status();
    }
    auto retirement_result = PageRetirementReservation::reserve();
    if (!retirement_result) return retirement_result.status();
    auto retirement = std::move(retirement_result.value());
    // Stage before mm: the frames, the shared-file aliases and the file's
    // bytes (a provider read ranks below mm). Nothing here depends on where
    // the range lands.
    StagedRange staged;
    if (auto status = staged.stage(size, page_flags.value(), shared, shared_file, offset, source, file_size); !status)
        return status;
    // Build from the records as they are now: a sibling's munmap, mmap or
    // copy-on-write resolution meanwhile is part of this candidate.
    const sched::AddressSpaceGate::Hold hold(resources.mm);
    auto planned = plan();
    if (!planned) return planned.status();
    const uptr address = planned.value().address;
    const usize removed_count = planned.value().removed;
    Pages candidate;
    if (auto status = candidate.reserve(page_pool,
            resources.native_pages.size() - removed_count + size / hw::page_size, true); !status) return status;
    for (const auto &page : resources.native_pages)
        if (fixed && page.virtual_address >= address && page.virtual_address - address < size)
            continue;
        else
        {
            const auto *frame = owned_frame(resources.native_pages, page);
            if (!frame) return Status::fault("native mapping copy lost exact frame authority");
            if (auto status = candidate.push_back(page, *frame); !status) return status;
        }
    auto rollback_pages = [&] {
        if (auto status = retirement.release_range(candidate, address, size); !status)
            runtime_failure = status;
    };
    // The staged frames move into the candidate; from here the candidate
    // owns them and the rollback releases its [address, address + size).
    if (auto status = staged.transfer(candidate, address); !status)
    {
        rollback_pages();
        return status;
    }
    bool can_release_staged = true;
    if (auto status = replace_native_pages(resources, candidate, can_release_staged, address, size); !status.ok())
    {
        if (can_release_staged)
            rollback_pages();
        else if (!candidate.empty())
            // A post-publication failure leaves the retired prior directory
            // here. Quarantine failures instead transfer and empty candidate.
            if (auto retired = retirement.release_range(candidate, address, size); !retired)
                runtime_failure = retired;
        return status;
    }
    // Fixed replacement keeps old backing live until old page tables retire.
    // Failed staging leaves both the prior bytes and permissions untouched.
    if (fixed)
        if (auto status = retirement.release_range(candidate, address, size); !status)
            runtime_failure = status;
    if (auto status = candidate.clear(); !status) runtime_failure = status;
    return address;
}

Status native_mprotect(sched::ProcessResources &resources, uptr address, usize length, uptr protection)
{
    const sched::AddressSpaceGate::Hold hold(resources.mm);
    if (resources.mm.dead)
        return Status::no_process("native address space was torn down");
    auto rounded = native_page_length(length);
    if (!rounded)
        return rounded.status();
    if (!valid_native_range(address, rounded.value()))
        return Status::invalid_argument("native protection range is invalid");
    if (private_signal_reserved_range(resources, address, rounded.value()))
        return Status::denied("private signal return page is reserved");
    auto flags = native_page_flags(protection, protection == posix::prot_WRITE);
    if (!flags) return flags.status();
    // Complete authority/protection admission before staging any changed page
    // list or hardware table. A refusal preserves the entire original range.
    usize admitted = 0;
    for (const auto &page : resources.native_pages)
        if (page.virtual_address >= address && page.virtual_address - address < rounded.value())
        {
            const auto *frame = owned_frame(resources.native_pages, page);
            if (!frame) return Status::fault("native protection lost exact frame authority");
            const bool shared_file = shared_files.owns(*frame);
            if (protection == posix::prot_WRITE && !shared_file)
                return Status::unsupported("write-only requests require the sealed shared-file adapter");
            if ((protection & posix::prot_WRITE) && shared_file)
                if (auto status = shared_files.check_mapping_write(*frame); !status) return status;
            ++admitted;
        }
    if (admitted != rounded.value() / hw::page_size)
        return Status::no_memory("native protection range contains unmapped pages");
    auto copied = Pages::clone(resources.native_pages, page_pool);
    if (!copied) return copied.status();
    Pages candidate = std::move(copied.value());
    usize changed = 0;
    for (auto &page : candidate)
        if (page.virtual_address >= address && page.virtual_address - address < rounded.value())
        {
            settle_access(page, flags.value(), exclusive_frame(page_pool, page.physical_address));
            ++changed;
        }
    if (changed != rounded.value() / hw::page_size)
        return Status::no_memory("native protection range contains unmapped pages");
    bool can_release_staged = true;
    auto status = replace_native_pages(resources, candidate, can_release_staged);
    if (status)
        if (auto retired = candidate.clear(); !retired) runtime_failure = retired;
    return status;
}

Status native_munmap(sched::ProcessResources &resources, uptr address, usize length)
{
    const sched::AddressSpaceGate::Hold hold(resources.mm);
    if (resources.mm.dead)
        return Status::no_process("native address space was torn down");
    auto rounded = native_page_length(length);
    if (!rounded)
        return rounded.status();
    if (!valid_native_range(address, rounded.value()))
        return Status::invalid_argument("native unmap range is invalid");
    if (private_signal_reserved_range(resources, address, rounded.value()))
        return Status::denied("private signal return page is reserved");
    usize removed = 0;
    for (const auto &page : resources.native_pages)
        if (page.virtual_address >= address && page.virtual_address - address < rounded.value())
        {
            if (!owns_pool_page(resources.native_pages, page))
                return Status::fault("native unmap page has no pool owner");
            ++removed;
        }
    if (!removed)
        return Status::success();
    auto retirement_result = PageRetirementReservation::reserve();
    if (!retirement_result) return retirement_result.status();
    auto retirement = std::move(retirement_result.value());
    Pages candidate;
    if (auto status = candidate.reserve(page_pool, resources.native_pages.size() - removed, true); !status)
        return status;
    for (const auto &page : resources.native_pages)
        if (page.virtual_address < address || page.virtual_address - address >= rounded.value())
        {
            const auto *frame = owned_frame(resources.native_pages, page);
            if (!frame) return Status::fault("native unmap copy lost exact frame authority");
            if (auto status = candidate.push_back(page, *frame); !status) return status;
        }
    bool can_release_staged = true;
    if (auto status = replace_native_pages(resources, candidate, can_release_staged); !status.ok())
    {
        if (!can_release_staged && !candidate.empty())
            if (auto retired = retirement.release_range(candidate, address, rounded.value()); !retired)
                runtime_failure = retired;
        return status;
    }
    // No old user translations can run after successful table retirement.
    if (auto status = retirement.release_range(candidate, address, rounded.value()); !status)
        runtime_failure = status;
    if (auto status = candidate.clear(); !status) runtime_failure = status;
    return Status::success();
}

// The whole call holds mm (the break and the records it checks); its mmap
// stages anonymous frames under it, which ranks allow.
Result<uptr> native_brk(sched::ProcessResources &resources, uptr address)
{
    const sched::AddressSpaceGate::Hold hold(resources.mm);
    if (!address)
        return resources.program_break;
    if (resources.program_break_base < hw::user_begin ||
        resources.program_break_limit >= hw::user_end ||
        address < resources.program_break_base || address > resources.program_break_limit)
        return Status::no_memory("native program break outside heap window");
    if (address - resources.program_break_base > resources.memory_limits.data.current)
        return Status::no_memory("process data quota exceeded");
    const uptr old_end = (resources.program_break + hw::page_size - 1) & ~(hw::page_size - 1);
    const uptr new_end = (address + hw::page_size - 1) & ~(hw::page_size - 1);
    if (new_end > old_end)
    {
        // Unlike MAP_FIXED, brk must never replace an intervening mapping.
        for (const auto &page : resources.native_pages)
            if (page.virtual_address >= old_end && page.virtual_address < new_end)
                return Status::no_memory("native heap growth overlaps an existing mapping");
        auto mapped = native_mmap(resources, old_end, new_end - old_end,
                                  posix::prot_READ | posix::prot_WRITE,
                                  posix::map_PRIVATE | posix::map_ANONYMOUS | posix::map_FIXED,
                                  ~uptr{0}, 0);
        if (!mapped)
            return mapped.status();
    }
    else if (new_end < old_end)
        if (auto status = native_munmap(resources, new_end, old_end - new_end); !status.ok())
            return status;
    resources.program_break = address;
    return address;
}

Status store(Pages &pages, uptr address, const void *data, usize count)
{
    const auto *bytes = static_cast<const std::byte *>(data);
    while (count)
    {
        hw::PageMapping *mapping = nullptr;
        for (auto &page : pages)
            if (page.virtual_address == (address & ~(hw::page_size - 1)))
            {
                mapping = &page;
                break;
            }
        if (!mapping)
            return Status::fault("image destination is unmapped");
        usize offset = address & (hw::page_size - 1);
        usize chunk = hw::page_size - offset;
        if (chunk > count)
            chunk = count;
        copy_bytes(reinterpret_cast<void *>(mapping->physical_address + offset), bytes, chunk);
        address += chunk;
        bytes += chunk;
        count -= chunk;
    }
    return Status::success();
}
i64 fork_process(Kernel &kernel, sched::ProcessId pid, sched::ProcessControlBlock *process,
                 [[maybe_unused]] sched::ThreadId calling_tid)
{
    auto result = kernel.posix().fork(pid, false);
    if (!result)
        return error(result.status());
    const auto child_pid = result.value();
    auto *child = kernel.scheduler().find(child_pid);
    if (auto status = prepare_native_thread(child->threads()[0]); !status)
    {
        finish(kernel, child_pid, 127);
        static_cast<void>(kernel.posix().wait(pid, child_pid));
        return error(status);
    }
    // The copy runs under the parent's mm (its records and translations); a
    // failure leaves it before the child is finished, which takes the child's.
    bool fatal = false;
    const auto copied = [&]() -> Status {
        auto &parent = process->resources();
        const sched::AddressSpaceGate::Hold hold(parent.mm);
        auto &pages = child->resources().native_pages;
        if (auto status = pages.reserve(page_pool, parent.native_pages.size(), true); !status) return status;
        for (const auto &page : parent.native_pages)
        {
            const auto *source_frame = owned_frame(parent.native_pages, page);
            if (!source_frame) return Status::fault("fork page lost exact frame authority");
            if (shared_pool_page(parent.native_pages, page))
            {
                const auto owner = *source_frame;
                if (auto status = retain_page_reference(owner); !status) return status;
                if (auto status = pages.push_back(page, owner); !status)
                {
                    static_cast<void>(release_page_reference(owner));
                    return status;
                }
                continue;
            }
            if (auto status = copy_on_write_fork ? share_private_page(pages, page, *source_frame, page_pool)
                                                 : allocate(pages, page.virtual_address, page.flags); !status.ok())
                return status;
            if (!copy_on_write_fork)
                copy_bytes(reinterpret_cast<void *>(pages[pages.size() - 1].physical_address),
                           reinterpret_cast<const void *>(source_frame->address), hw::page_size);
        }
        sched::MemoryMap child_memory;
        if (auto status = page_memory_map(pages, child_memory); !status) return status;
        auto child_space = create_page_space(pages);
        if (!child_space) return child_space.status();
        // The child is not runnable yet and no other CPU can name it: its
        // handle is stored without its own mm (one mm per CPU).
        child->resources().native_space = child_space.value();
        child->resources().memory = child_memory;
#ifdef OK_NATIVE_COW_FORK
        // One protect over the parent's records: one quiesce.
        if (auto status = withhold_shared_writes(parent.native_pages, parent.native_space, page_pool); !status)
        {
            fatal = true;
            return status;
        }
#endif
        return {};
    }();
    if (fatal) { runtime_failure = copied; started = false; return error(copied); }
    if (!copied)
    {
        finish(kernel, child_pid, 127);
        static_cast<void>(kernel.posix().wait(pid, child_pid));
        return error(copied);
    }
    if (auto status = encode_result(*child, child->threads()[0].native_frame, 0); !status.ok())
    {
        finish(kernel, child_pid, 127);
        static_cast<void>(kernel.posix().wait(pid, child_pid));
        return error(status);
    }
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
    if (auto status = private_signal_fork(kernel, pid, calling_tid, child_pid); !status)
    {
        finish(kernel, child_pid, 127);
        static_cast<void>(kernel.posix().wait(pid, child_pid));
        return error(status);
    }
#endif
    child->threads()[0].native_active = true;
    child->threads()[0].syscall_pending = false;
    if (auto status = kernel.scheduler().set_runnable(child_pid); !status)
    {
        finish(kernel, child_pid, 127);
        static_cast<void>(kernel.posix().wait(pid, child_pid));
        return error(status);
    }
    return static_cast<i64>(child_pid);
}
} // namespace runtime_detail
Status prepare_memory()
{
#ifdef OK_KERNEL_FREESTANDING
    if (memory_preparation_attempted) return memory_preparation;
    memory_preparation_attempted = true;
    // Discovery belongs to kernel boot; the runtime only requires its result.
    memory_preparation = boot::discover_kernel_memory(emit).status;
    return memory_preparation;
#else
    return Status::unsupported("hosted tests do not discover firmware");
#endif
}
} // namespace ok::user::native
// Backend translation tables are kernel objects of the native page pool.
ok::Result<ok::uptr> ok::arch::execution::allocate_table_pages(usize pages, usize alignment)
{ return user::native::page_pool.allocate_object_run(pages, alignment); }
ok::Status ok::arch::execution::release_table_pages(uptr address, usize pages)
{ return user::native::page_pool.release_object_run(address, pages); }
