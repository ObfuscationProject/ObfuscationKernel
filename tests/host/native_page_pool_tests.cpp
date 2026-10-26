#include "ok/user/native_page_pool.hpp"
#include "ok/memory/object_pool.hpp"
#include <cstdio>
#include <cstdlib>
#include <type_traits>
using namespace ok;
using user::native::NativePagePool;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::abort(); } } while(false)
void original_registry_checks()
{
    static memory::FrameAllocator frames;
    static memory::FrameMetadataStorage<2048> frames_metadata;
    static NativePagePool pool;
    CHECK(!pool.allocate());
    CHECK(!pool.activate(frames,NativePagePool::Source::qualified_firmware));
    memory::MemoryRegion region{0x100000,1024*4096,memory::RegionType::usable};
    CHECK(frames.initialize({&region,1}, 4096, frames_metadata.bytes));
    CHECK(pool.activate(frames,NativePagePool::Source::qualified_firmware));
    CHECK(!pool.activate(frames,NativePagePool::Source::linker_reserved));
    std::array<uptr,1024> addresses{};
    for (auto &address : addresses)
    {
        auto allocated=pool.allocate();CHECK(allocated);address=allocated.value();
        const auto handle=pool.handle(address);
        CHECK(handle && handle->owner==&frames && handle->generation!=0 && pool.owns(address));
    }
    CHECK(!pool.allocate() && pool.used_count()==1024 && pool.peak_count()==1024);
    const auto stale=*pool.handle(addresses[0]);
    CHECK(pool.pin(addresses[0]) && pool.pin(addresses[0]));
    CHECK(pool.release(addresses[0]) && pool.used_count()==1024);
    CHECK(pool.unpin(addresses[0]) && pool.used_count()==1024);
    CHECK(pool.unpin(addresses[0]) && pool.used_count()==1023);
    CHECK(!pool.release(addresses[0]) && !pool.unpin(addresses[0]));
    auto reused=pool.allocate();CHECK(reused && reused.value()==addresses[0]);
    CHECK(pool.handle(reused.value())->generation!=stale.generation);
    CHECK(!frames.release(stale) && pool.used_count()==1024);
    for (auto address : addresses) CHECK(pool.release(address));
    CHECK(pool.used_count()==0 && pool.releases()==1025 && pool.allocations()==1025);
    CHECK(!pool.release(0x1234) && !pool.pin(0x1234));
    CHECK(pool.user_count()==0 && pool.object_count()==0 && pool.free_count()==1024);
    CHECK(pool.kernel_reserve_count()==0 && pool.user_capacity_count()==1024);
    CHECK(!pool.allocate_kernel_page());
}

namespace
{
alignas(NativePagePool::page_size) std::array<std::byte, 8*NativePagePool::page_size> backing{};
alignas(NativePagePool::page_size) std::array<std::byte, 2*NativePagePool::page_size> other_backing{};
void account(const NativePagePool &pool, usize users, usize objects, usize free, usize pinned=0, usize pending=0)
{
    CHECK(pool.user_count()==users && pool.object_count()==objects && pool.free_count()==free);
    CHECK(pool.used_count()==users+objects && pool.total_count()==users+objects+free);
    CHECK(pool.pinned_count()==pinned && pool.pending_count()==pending);
    CHECK(pending<=pinned && pinned<=users);
    CHECK(pool.allocations()-pool.releases()==pool.used_count());
}
void object_reserve_and_lease_checks()
{
    static memory::FrameAllocator frames,other_frames;
    static memory::FrameMetadataStorage<2048> frames_metadata;
    static memory::FrameMetadataStorage<2048> other_frames_metadata;
    static NativePagePool pool,other;
    static_assert(NativePagePool::page_size==4096);
    static_assert(memory::ObjectPageProvider<NativePagePool>);
    static_assert(std::is_default_constructible_v<NativePagePool::Lease>);
    static_assert(!std::is_copy_constructible_v<NativePagePool::Lease>);
    static_assert(std::is_nothrow_move_constructible_v<NativePagePool::Lease>);
    CHECK(!pool.allocate_kernel_page());
    NativePagePool::Lease empty;
    CHECK(!pool.release_kernel_page(empty));
    const memory::MemoryRegion region{reinterpret_cast<uptr>(backing.data()),backing.size(),memory::RegionType::usable};
    const memory::MemoryRegion second{reinterpret_cast<uptr>(other_backing.data()),other_backing.size(),memory::RegionType::usable};
    CHECK(frames.initialize({&region,1}, NativePagePool::page_size, frames_metadata.bytes));
    CHECK(other_frames.initialize({&second,1}, NativePagePool::page_size, other_frames_metadata.bytes));
    CHECK(!pool.activate(frames,NativePagePool::Source::linker_reserved,9));
    CHECK(!pool.activate(frames,static_cast<NativePagePool::Source>(255),3));
    CHECK(pool.source()==NativePagePool::Source::inactive && frames.free_frames()==8);
    CHECK(pool.activate(frames,NativePagePool::Source::linker_reserved,3));
    CHECK(other.activate(other_frames,NativePagePool::Source::linker_reserved,1));
    CHECK(pool.kernel_reserve_count()==3 && pool.user_capacity_count()==5);
    account(pool,0,0,8);
    std::array<uptr,5> users{};
    for(auto &address:users){auto value=pool.allocate();CHECK(value);address=value.value();}
    CHECK(pool.allocate().status().code()==StatusCode::no_memory);
    account(pool,5,0,3);
    std::array<NativePagePool::Lease,3> leases{};
    for(usize i=0;i<leases.size();++i)
    {
        auto value=pool.allocate_kernel_page();CHECK(value);leases[i]=std::move(value.value());
        CHECK(!value.value().bytes.data());
        auto &lease=leases[i];const auto address=reinterpret_cast<uptr>(lease.bytes.data());
        CHECK(lease.bytes.size()==NativePagePool::page_size && address%NativePagePool::page_size==0);
        CHECK(address>=region.base && address+lease.bytes.size()<=region.base+region.size);
        for(auto user:users) CHECK(user!=address);
        for(usize previous=0;previous<i;++previous) CHECK(leases[previous].bytes.data()!=lease.bytes.data());
        for(usize byte=0;byte<lease.bytes.size();++byte)lease.bytes[byte]=std::byte(i+1);
        CHECK(!pool.owns(address) && !pool.handle(address));
        CHECK(!pool.release(address) && !pool.pin(address) && !pool.unpin(address));
        CHECK(!other.release_kernel_page(lease));
    }
    account(pool,5,3,0);
    CHECK(!pool.allocate() && !pool.allocate_kernel_page());
    for(usize i=0;i<leases.size();++i)for(auto byte:leases[i].bytes) CHECK(byte==std::byte(i+1));
    auto original_bytes=leases[0].bytes;
    leases[0].bytes=leases[0].bytes.subspan(1);CHECK(!pool.release_kernel_page(leases[0]));
    leases[0].bytes={leases[1].bytes.data(),NativePagePool::page_size};CHECK(!pool.release_kernel_page(leases[0]));
    leases[0].bytes=original_bytes;
    NativePagePool::Lease fake;fake.bytes={reinterpret_cast<std::byte*>(users[0]),NativePagePool::page_size};
    CHECK(!pool.release_kernel_page(fake));account(pool,5,3,0);
    CHECK(pool.pin(users[0]) && pool.pin(users[0]));CHECK(pool.release(users[0]));
    account(pool,5,3,0,1,1);CHECK(!pool.allocate());
    CHECK(pool.unpin(users[0]));account(pool,5,3,0,1,1);
    CHECK(pool.unpin(users[0]));account(pool,4,3,1);
    CHECK(pool.release_kernel_page(leases[0]));account(pool,4,2,2);
    CHECK(!pool.release_kernel_page(leases[0]));
    auto replacement=pool.allocate_kernel_page();CHECK(replacement);account(pool,4,3,1);
    CHECK(!pool.release_kernel_page(leases[0]));
    CHECK(pool.release_kernel_page(replacement.value()));
    for(usize i=1;i<leases.size();++i)CHECK(pool.release_kernel_page(leases[i]));
    account(pool,4,0,4);
    // Reserve cannot be borrowed by users when all object pages are returned.
    auto user=pool.allocate();CHECK(user);CHECK(!pool.allocate());
    account(pool,5,0,3);CHECK(pool.release(user.value()));
    for(usize i=1;i<users.size();++i)CHECK(pool.release(users[i]));
    account(pool,0,0,8);
    // Reusing the same frame must invalidate the old lease generation.
    auto once=pool.allocate_kernel_page();CHECK(once);const auto address=once.value().bytes.data();
    CHECK(pool.release_kernel_page(once.value()));
    auto again=pool.allocate_kernel_page();CHECK(again && again.value().bytes.data()==address);
    CHECK(!pool.release_kernel_page(once.value()));CHECK(pool.release_kernel_page(again.value()));
    account(pool,0,0,8);
    // An entirely reserved pool offers no user quota but still supplies objects.
    static memory::FrameAllocator reserved_frames;
    static memory::FrameMetadataStorage<2048> reserved_frames_metadata;static NativePagePool reserved;
    alignas(NativePagePool::page_size) static std::array<std::byte,NativePagePool::page_size> last{};
    const memory::MemoryRegion reserve_region{reinterpret_cast<uptr>(last.data()),last.size(),memory::RegionType::usable};
    CHECK(reserved_frames.initialize({&reserve_region,1}, NativePagePool::page_size, reserved_frames_metadata.bytes));
    CHECK(reserved.activate(reserved_frames,NativePagePool::Source::linker_reserved,1));
    CHECK(!reserved.allocate() && reserved.user_capacity_count()==0);
    auto all=reserved.allocate_kernel_page();CHECK(all);CHECK(reserved.release_kernel_page(all.value()));
    account(reserved,0,0,1);
    // Address-model checks preserve the entire supplied MIPS supervisor alias;
    // these bytes are never dereferenced on the hosted machine.
    static memory::FrameAllocator alias_frames;
    static memory::FrameMetadataStorage<2048> alias_frames_metadata;static NativePagePool aliases;
    const uptr alias=static_cast<uptr>(static_cast<__INTPTR_TYPE__>(-2147483648LL))+0x100000;
    const memory::MemoryRegion alias_region{alias,NativePagePool::page_size,memory::RegionType::usable};
    CHECK(alias_frames.initialize({&alias_region,1}, NativePagePool::page_size, alias_frames_metadata.bytes));
    CHECK(aliases.activate(alias_frames,NativePagePool::Source::linker_reserved,1));
    auto alias_lease=aliases.allocate_kernel_page();CHECK(alias_lease);
    CHECK(reinterpret_cast<uptr>(alias_lease.value().bytes.data())==alias);
    CHECK(!aliases.owns(alias) && !aliases.owns(alias&0x1fffffffu));
    CHECK(aliases.release_kernel_page(alias_lease.value()));account(aliases,0,0,1);
    // A null kernel backing span is rejected and its exact frame is refunded.
    static memory::FrameAllocator null_frames;
    static memory::FrameMetadataStorage<2048> null_frames_metadata;static NativePagePool null_pool;
    const memory::MemoryRegion null_region{0,NativePagePool::page_size,memory::RegionType::usable};
    CHECK(null_frames.initialize({&null_region,1}, NativePagePool::page_size, null_frames_metadata.bytes));
    CHECK(null_pool.activate(null_frames,NativePagePool::Source::linker_reserved,1));
    CHECK(null_pool.allocate_kernel_page().status().code()==StatusCode::fault);
    account(null_pool,0,0,1);CHECK(null_pool.allocations()==1 && null_pool.releases()==1);
}
void dropped_and_overwritten_leases_remain_auditable()
{
    alignas(NativePagePool::page_size) static std::array<std::byte, 2 * NativePagePool::page_size> storage{};
    static memory::FrameAllocator frames;
    static memory::FrameMetadataStorage<2048> frames_metadata;
    static NativePagePool pool;
    const memory::MemoryRegion region{reinterpret_cast<uptr>(storage.data()), storage.size(),
                                      memory::RegionType::usable};
    CHECK(frames.initialize({&region, 1}, NativePagePool::page_size, frames_metadata.bytes));
    CHECK(pool.activate(frames, NativePagePool::Source::linker_reserved, 2));
    usize dropped_index = frames.total_frames();
    {
        auto first = pool.allocate_kernel_page();
        auto second = pool.allocate_kernel_page();
        CHECK(first && second);
        const auto original = first.value().bytes;
        first.value().bytes = first.value().bytes.subspan(1);
        CHECK(!pool.release_kernel_page(first.value()));
        first.value().bytes = original;
        const auto first_address = reinterpret_cast<uptr>(first.value().bytes.data());
        first.value() = std::move(second.value());
        CHECK(!second.value().bytes.data() && pool.quarantined_kernel_page_count() == 1);
        for (usize index = 0; index < frames.total_frames(); ++index)
            if (const auto frame = pool.quarantined_kernel_page(index))
            {
                CHECK(frame->address == first_address && frame->owner == &frames);
                dropped_index = index;
            }
        CHECK(dropped_index < frames.total_frames());
        // The remaining active lease is retained automatically on scope exit.
    }
    CHECK(pool.object_count() == 2 && pool.quarantined_kernel_page_count() == 2);
    CHECK(pool.quarantined_kernel_page(dropped_index)->generation != 0);
    CHECK(frames.free_frames() == 0 && !pool.allocate_kernel_page());
}
void shared_process_references_are_distinct_from_retirement_pins()
{
    alignas(NativePagePool::page_size) static std::array<std::byte, 2 * NativePagePool::page_size> storage{};
    static memory::FrameAllocator frames;
    static memory::FrameMetadataStorage<2048> frames_metadata;
    static NativePagePool pool;
    const memory::MemoryRegion region{reinterpret_cast<uptr>(storage.data()), storage.size(),
                                      memory::RegionType::usable};
    CHECK(frames.initialize({&region, 1}, NativePagePool::page_size, frames_metadata.bytes));
    CHECK(pool.activate(frames, NativePagePool::Source::qualified_firmware));
    auto first = pool.allocate();
    CHECK(first && !pool.is_shared(first.value()) && pool.reference_count(first.value()) == 1);
    const auto original = *pool.handle(first.value());
    CHECK(!pool.retain_shared(original) && pool.reference_count(first.value()) == 1);
    CHECK(pool.mark_shared(original) && pool.is_shared(first.value()));
    CHECK(pool.retain_shared(original) && pool.reference_count(first.value()) == 2);
    CHECK(pool.release_reference(original) && pool.reference_count(first.value()) == 1);
    CHECK(pool.is_shared(first.value())); // Classification survives a single owner.
    CHECK(pool.retain_shared(original) && pool.reference_count(first.value()) == 2);
    CHECK(pool.pin(first.value()));
    CHECK(pool.release_reference(original) && pool.reference_count(first.value()) == 1);
    CHECK(pool.release_reference(original) && pool.reference_count(first.value()) == 0);
    CHECK(pool.pending_count() == 1 && pool.pinned_count() == 1 && pool.used_count() == 1);
    CHECK(!pool.retain_shared(original) && !pool.release_reference(original));
    CHECK(pool.reference_count(first.value()) == 0);
    CHECK(pool.unpin(first.value()));
    CHECK(pool.pending_count() == 0 && pool.pinned_count() == 0 && pool.used_count() == 0);
    auto reused = pool.allocate();
    CHECK(reused && reused.value() == first.value());
    CHECK(pool.handle(reused.value())->generation != original.generation);
    CHECK(!pool.mark_shared(original) && !pool.retain_shared(original) && !pool.release_reference(original));
    CHECK(pool.reference_count(reused.value()) == 1 && !pool.is_shared(reused.value()));
    auto forged = *pool.handle(reused.value());
    ++forged.generation;
    CHECK(!pool.mark_shared(forged) && !pool.retain_shared(forged) && !pool.release_reference(forged));
    CHECK(pool.mark_shared(*pool.handle(reused.value())));
    const auto live = *pool.handle(reused.value());
    for (usize count = 1; count < 65535; ++count) CHECK(pool.retain_shared(live));
    CHECK(pool.reference_count(reused.value()) == 65535 && !pool.retain_shared(live));
    CHECK(pool.reference_count(reused.value()) == 65535 && pool.pinned_count() == 0);
    for (usize count = 0; count < 65535; ++count) CHECK(pool.release_reference(live));
    CHECK(pool.used_count() == 0 && pool.free_count() == 2 && pool.pending_count() == 0);
}
void copy_on_write_references_stay_private()
{
    alignas(NativePagePool::page_size) static std::array<std::byte, 2 * NativePagePool::page_size> storage{};
    static memory::FrameAllocator frames;
    static memory::FrameMetadataStorage<2048> frames_metadata;
    static NativePagePool pool;
    const memory::MemoryRegion region{reinterpret_cast<uptr>(storage.data()), storage.size(),
                                      memory::RegionType::usable};
    CHECK(frames.initialize({&region, 1}, NativePagePool::page_size, frames_metadata.bytes));
    CHECK(pool.activate(frames, NativePagePool::Source::qualified_firmware));
    auto first = pool.allocate();
    CHECK(first);
    const auto owner = *pool.handle(first.value());
    CHECK(pool.retain_copy(owner) && pool.reference_count(first.value()) == 2 && !pool.is_shared(first.value()));
    // Copy-on-write sharing never classifies the frame shared, nor the reverse.
    CHECK(!pool.retain_shared(owner) && !pool.mark_shared(owner));
    CHECK(pool.release_reference(owner) && pool.reference_count(first.value()) == 1);
    CHECK(pool.pin(owner) && pool.release_reference(owner) && pool.pending_count() == 1);
    CHECK(!pool.retain_copy(owner));
    CHECK(pool.unpin(owner) && pool.used_count() == 0);
    CHECK(!pool.retain_copy(owner));
    auto shared = pool.allocate_frame();
    CHECK(shared && pool.mark_shared(shared.value()) && !pool.retain_copy(shared.value()));
    CHECK(pool.release_reference(shared.value()));
    auto live = pool.allocate_frame();
    CHECK(live);
    for (usize count = 1; count < 65535; ++count) CHECK(pool.retain_copy(live.value()));
    CHECK(pool.reference_count(live.value().address) == 65535 && !pool.retain_copy(live.value()));
    for (usize count = 0; count < 65535; ++count) CHECK(pool.release_reference(live.value()));
    CHECK(pool.used_count() == 0 && pool.free_count() == 2);
}
void failed_final_release_preserves_retry_state()
{
    alignas(NativePagePool::page_size) static std::array<std::byte, NativePagePool::page_size> storage{};
    static memory::FrameAllocator frames;
    static memory::FrameMetadataStorage<2048> frames_metadata;
    static NativePagePool pool;
    const memory::MemoryRegion region{reinterpret_cast<uptr>(storage.data()), storage.size(),
                                      memory::RegionType::usable};
    CHECK(frames.initialize({&region, 1}, NativePagePool::page_size, frames_metadata.bytes));
    CHECK(pool.activate(frames, NativePagePool::Source::qualified_firmware));
    auto page = pool.allocate();
    CHECK(page);
    const auto address = page.value();
    // The registry keeps no copy of the handle: its state is the allocator's
    // descriptor. A descriptor that no longer names this pool's kind refuses
    // release and leaves accounting intact; restoring it allows the retry.
    const auto owner = *pool.handle(address);
    auto *descriptor = frames.descriptor(owner);
    CHECK(descriptor);
    const auto kind = descriptor->kind;
    descriptor->kind = static_cast<u8>(memory::PageKind::native_object);
    CHECK(!pool.release(address) && !pool.release_reference(owner));
    CHECK(!pool.owns(address) && pool.used_count() == 1 && pool.free_count() == 0 && pool.pending_count() == 0);
    descriptor->kind = kind;
    CHECK(pool.owns(address) && pool.reference_count(address) == 1);
    CHECK(pool.release(address));
    CHECK(pool.used_count() == 0 && pool.free_count() == 1);

    page = pool.allocate();
    CHECK(page && page.value() == address);
    CHECK(pool.pin(address) && pool.release(address));
    CHECK(pool.reference_count(address) == 0 && pool.pending_count() == 1 && pool.pinned_count() == 1);
    const auto pinned = *pool.handle(address);
    auto stale = pinned;
    ++stale.generation;
    CHECK(!pool.unpin(stale));
    CHECK(pool.owns(address) && pool.reference_count(address) == 0);
    CHECK(pool.used_count() == 1 && pool.free_count() == 0);
    CHECK(pool.pending_count() == 1 && pool.pinned_count() == 1);
    CHECK(pool.unpin(address));
    CHECK(pool.used_count() == 0 && pool.free_count() == 1);
    CHECK(pool.pending_count() == 0 && pool.pinned_count() == 0);
}
void exact_generation_wait_pin_survives_mapping_retirement()
{
    alignas(NativePagePool::page_size) static std::array<std::byte, NativePagePool::page_size> storage{};
    static memory::FrameAllocator frames;
    static memory::FrameMetadataStorage<2048> frames_metadata;
    static NativePagePool pool;
    const memory::MemoryRegion region{reinterpret_cast<uptr>(storage.data()), storage.size(),
                                      memory::RegionType::usable};
    CHECK(frames.initialize({&region, 1}, NativePagePool::page_size, frames_metadata.bytes));
    CHECK(pool.activate(frames, NativePagePool::Source::qualified_firmware));
    auto page = pool.allocate(); CHECK(page);
    const uptr address = page.value();
    const auto owner = *pool.handle(address);
    CHECK(pool.mark_shared(owner) && pool.retain_shared(owner));
    CHECK(pool.pin(owner) && pool.pinned_count() == 1);
    CHECK(pool.release_reference(owner) && pool.reference_count(address) == 1);
    CHECK(pool.release_reference(owner) && pool.reference_count(address) == 0);
    CHECK(pool.pending_count() == 1 && pool.pinned_count() == 1);
    CHECK(!pool.pin(owner)); // A final mapping owner cannot be resurrected.
    auto stale = owner; ++stale.generation;
    CHECK(!pool.unpin(stale) && pool.pending_count() == 1 && pool.pinned_count() == 1);
    CHECK(pool.unpin(owner) && pool.pending_count() == 0 && pool.pinned_count() == 0);
    page = pool.allocate(); CHECK(page && page.value() == address);
    CHECK(pool.handle(address)->generation != owner.generation);
    CHECK(!pool.pin(owner) && !pool.unpin(owner));
    CHECK(pool.reference_count(address) == 1 && pool.pinned_count() == 0);
    CHECK(pool.release(address) && pool.used_count() == 0);
}
}
void shared_allocator_form_respects_other_owners()
{
    alignas(NativePagePool::page_size) static std::array<std::byte, 8 * NativePagePool::page_size> storage{};
    static memory::FrameAllocator frames;
    static memory::FrameMetadataStorage<8> frames_metadata;
    static NativePagePool pool;
    const memory::MemoryRegion region{reinterpret_cast<uptr>(storage.data()), storage.size(),
                                      memory::RegionType::usable};
    CHECK(frames.initialize({&region, 1}, NativePagePool::page_size, frames_metadata.bytes));
    // Another kernel owner already holds a frame; the exclusive form refuses.
    memory::ChargeAccount kernel_owner;
    auto foreign = frames.allocate(kernel_owner);
    CHECK(foreign && !pool.activate(frames, NativePagePool::Source::qualified_firmware, 1));
    CHECK(!pool.activate(frames, NativePagePool::Source::qualified_firmware, 2, 7));
    CHECK(pool.activate(frames, NativePagePool::Source::qualified_firmware, 2, 3));
    CHECK(pool.kernel_reserve_count() == 2 && pool.user_capacity_count() == 3 && pool.total_count() == 8);
    CHECK(!pool.owns(foreign.value().address) && !pool.valid(foreign.value()) && !pool.pin(foreign.value()));
    CHECK(!pool.release(foreign.value().address) && frames.live(foreign.value()));
    std::array<uptr, 3> users{};
    for (auto &address : users)
    {
        auto page = pool.allocate();
        CHECK(page && page.value() != foreign.value().address);
        address = page.value();
    }
    CHECK(pool.allocate().status().code() == StatusCode::no_memory && frames.free_frames() == 4);
    auto object = pool.allocate_kernel_page();
    CHECK(object && pool.object_count() == 1 && pool.free_count() == 3);
    // The other owner's allocation and release do not disturb pool state.
    CHECK(frames.release(foreign.value(), kernel_owner) && pool.used_count() == 4);
    for (auto address : users) CHECK(pool.release(address));
    CHECK(pool.release_kernel_page(object.value()) && pool.used_count() == 0 && frames.free_frames() == 8);
}
int main()
{
    original_registry_checks();object_reserve_and_lease_checks();
    dropped_and_overwritten_leases_remain_auditable();
    shared_process_references_are_distinct_from_retirement_pins();
    copy_on_write_references_stay_private();
    failed_final_release_preserves_retry_state();
    exact_generation_wait_pin_survives_mapping_retirement();
    shared_allocator_form_respects_other_owners();
    std::puts("PASS native page provider: user/object ownership, copy-on-write references, opaque lease generations, exact backing, reserve quota, accounting and retained quarantine; hosted=1 guest=0 smp=0");
}
