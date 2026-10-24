#include "ok/user/native_mapping_pages.hpp"
#include <cstdio>
#include <cstdlib>
#include <type_traits>

using namespace ok;
using ok::user::native::MappingPages;
using ok::user::native::NativePagePool;

namespace ok::user::native
{
struct MappingPagesFaultProbe
{
    static void invalidate_last_span(MappingPages &owner)
    {
        auto &bytes = owner.leases_[owner.blocks_ - 1].bytes;
        bytes = bytes.subspan(1);
    }
};
}

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "mapping-pages:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)

static_assert(!std::is_copy_constructible_v<MappingPages>);
static_assert(std::is_nothrow_move_constructible_v<MappingPages>);
static_assert(MappingPages::capacity == 1024);
static_assert(MappingPages::records_per_block == 170);
static_assert(MappingPages::max_blocks == 7);

namespace
{
alignas(4096) std::array<std::byte, 42 * 4096> failure_backing{};
std::array<memory::FrameAllocator, 6> failure_frames{};
std::array<NativePagePool, 6> failure_pools{};
alignas(4096) std::array<std::byte, 14 * 4096> success_backing{};
memory::FrameAllocator success_frames;
NativePagePool success_pool;
alignas(4096) std::array<std::byte, 2 * 4096> failed_backing{};
memory::FrameAllocator failed_frames;
NativePagePool failed_pool;

void block_boundary_failures()
{
    for (usize available = 1; available <= 6; ++available)
    {
        auto &frames = failure_frames[available - 1];
        auto &pool = failure_pools[available - 1];
        const memory::MemoryRegion region{
            reinterpret_cast<uptr>(failure_backing.data() + (available - 1) * 7 * 4096),
            available * 4096, memory::RegionType::usable};
        CHECK(frames.initialize({&region, 1}, 4096));
        CHECK(pool.activate(frames, NativePagePool::Source::linker_reserved, available));
        MappingPages owner;
        CHECK(owner.reserve(pool, available * MappingPages::records_per_block));
        CHECK(owner.block_count() == available && pool.object_count() == available);
        CHECK(!owner.reserve(pool, available * MappingPages::records_per_block + 1));
        CHECK(owner.block_count() == available && owner.empty() && pool.object_count() == available);
        CHECK(owner.clear() && pool.object_count() == 0 && frames.free_frames() == available);
        CHECK(!owner.reserve(pool, MappingPages::capacity + 1));
    }
}

void clone_and_relocation()
{
    const memory::MemoryRegion region{reinterpret_cast<uptr>(success_backing.data()),
        success_backing.size(), memory::RegionType::usable};
    CHECK(success_frames.initialize({&region, 1}, 4096));
    CHECK(success_pool.activate(success_frames, NativePagePool::Source::linker_reserved, 14));
    MappingPages first;
    CHECK(first.reserve(success_pool, MappingPages::capacity));
    for (usize index = MappingPages::capacity; index > 0; --index)
        CHECK(first.push_back({arch::execution::user_begin + (index - 1) * 4096,
                               0x2000 + index * 4096, static_cast<u8>(index == 171 ? 0 : 3)}));
    CHECK(first.size() == 1024 && first.block_count() == 7 && success_pool.object_count() == 7);
    auto before = first.view();
    CHECK(before.size() == 1024 && before[0].virtual_address == arch::execution::user_begin);
    CHECK(before[170].flags == 0 && before[1023].virtual_address == arch::execution::user_begin + 1023 * 4096);
    const auto first_block = before.chunks[0].data();
    MappingPages relocated = std::move(first);
    CHECK(first.empty() && first.block_count() == 0 && relocated.view().chunks[0].data() == first_block);
    auto clone = MappingPages::clone(relocated, success_pool);
    CHECK(clone && clone.value().size() == 1024 && success_pool.object_count() == 14);
    CHECK(clone.value().view().chunks[0].data() != first_block);
    MappingPages unavailable;
    CHECK(!unavailable.reserve(success_pool, 1) && unavailable.empty());
    CHECK(relocated[170].flags == 0 && relocated.view().chunks[0].data() == first_block);
    CHECK(clone.value().clear() && relocated.clear());
    CHECK(success_pool.object_count() == 0 && success_frames.free_frames() == 14);
}

void failed_final_release_keeps_canonical_tokens()
{
    const memory::MemoryRegion region{reinterpret_cast<uptr>(failed_backing.data()),
        failed_backing.size(), memory::RegionType::usable};
    CHECK(failed_frames.initialize({&region, 1}, 4096));
    CHECK(failed_pool.activate(failed_frames, NativePagePool::Source::linker_reserved, 2));
    {
        MappingPages owner;
        CHECK(owner.reserve(failed_pool, MappingPages::records_per_block + 1));
        CHECK(owner.block_count() == 2 && failed_pool.object_count() == 2);
        ok::user::native::MappingPagesFaultProbe::invalidate_last_span(owner);
        CHECK(!owner.clear() && owner.block_count() == 2);
        CHECK(failed_pool.quarantined_kernel_page_count() == 0);
    }
    CHECK(failed_pool.object_count() == 2 && failed_pool.quarantined_kernel_page_count() == 2);
    CHECK(failed_frames.free_frames() == 0 && !failed_pool.allocate_kernel_page());
    usize witnessed = 0;
    for (usize index = 0; index < NativePagePool::capacity; ++index)
        if (const auto *frame = failed_pool.quarantined_kernel_page(index))
        {
            CHECK(frame->owner == &failed_frames && frame->generation && frame->index == index);
            ++witnessed;
        }
    CHECK(witnessed == 2);
}
}

int main()
{
    block_boundary_failures();
    clone_and_relocation();
    failed_final_release_keeps_canonical_tokens();
    std::puts("PASS native mapping metadata: seven block boundaries, fallible clone, stable view, unique relocation and retained failed-release tokens; hosted=1 guest=0");
}
