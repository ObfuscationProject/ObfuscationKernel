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
    static void overstate_record_run(MappingPages &owner) { ++owner.record_pages_; }
};
}

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "mapping-pages:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)

static_assert(!std::is_copy_constructible_v<MappingPages>);
static_assert(std::is_nothrow_move_constructible_v<MappingPages>);
static_assert(MappingPages::capacity == arch::execution::user_window_pages);
static_assert(MappingPages::records_per_page == 4096 / (sizeof(arch::execution::PageMapping) + 1));

namespace
{
constexpr uptr page = 4096;

struct Arena
{
    memory::FrameAllocator frames;
    memory::FrameMetadataStorage<2048> metadata;
    NativePagePool pool;
    void activate(std::byte *backing, usize pages, usize reserve)
    {
        const memory::MemoryRegion region{reinterpret_cast<uptr>(backing), pages * page, memory::RegionType::usable};
        CHECK(frames.initialize({&region, 1}, page, metadata.bytes));
        CHECK(pool.activate(frames, NativePagePool::Source::linker_reserved, reserve));
    }
};

alignas(4096) std::array<std::byte, 40 * 4096> growth_backing{};
Arena growth;

void growth_moves_unpublished_runs_only()
{
    growth.activate(growth_backing.data(), growth_backing.size() / page, 32);
    MappingPages owner;
    CHECK(owner.reserve(growth.pool, 1) && owner.record_pages() == 1 && owner.frame_pages() == 0);
    CHECK(owner.reserved() == MappingPages::records_per_page && growth.pool.object_count() == 1);
    // Reverse insertion shifts every record; growth doubles the run.
    const usize count = 3 * MappingPages::records_per_page + 7;
    for (usize index = count; index > 0; --index)
        CHECK(owner.push_back({arch::execution::user_begin + (index - 1) * page, 0x2000 + index * page,
                               static_cast<u8>(index == 171 ? 0 : 3)}));
    CHECK(owner.size() == count && owner.reserved() >= count && owner.record_pages() == 4);
    CHECK(growth.pool.object_count() == 4);
    for (usize index = 0; index < count; ++index)
        CHECK(owner[index].virtual_address == arch::execution::user_begin + index * page && owner.retirement(index) == 0);
    CHECK(owner[170].flags == 0 && !owner.push_back({arch::execution::user_begin, 0x9000, 1}));

    // Retirement bytes move with their records on insertion and growth.
    owner.retirement(5) = 7;
    CHECK(owner.reserve(growth.pool, owner.reserved() + 1) && owner.record_pages() == 8 && owner.retirement(5) == 7);
    CHECK(owner.push_back({arch::execution::user_begin + count * page + page, 0x5000, 1}) && owner.retirement(5) == 7);

    // A published view pins the records: no insertion, no growth, replace only.
    const auto view = owner.view();
    CHECK(view.valid() && view.size() == owner.size() && view.records == &owner[0] && owner.published());
    CHECK(view.find(arch::execution::user_begin + 170 * page + 12) == &owner[170] && !view.find(arch::execution::user_end));
    CHECK(owner.push_back({arch::execution::user_begin + count * page, 0x6000, 1}).code() == StatusCode::busy);
    const usize reserved = owner.reserved();
    CHECK(owner.reserve(growth.pool, reserved + 1).code() == StatusCode::busy && owner.reserved() == reserved);

    MappingPages relocated = std::move(owner);
    CHECK(owner.empty() && owner.record_pages() == 0 && relocated.view().records == view.records);
    auto clone = MappingPages::clone(relocated, growth.pool);
    CHECK(clone && clone.value().size() == relocated.size() && !clone.value().published());
    CHECK(clone.value().view().records != view.records && clone.value()[170].flags == 0);
    CHECK(clone.value().clear() && relocated.clear() && !relocated.published());
    CHECK(growth.pool.object_count() == 0 && growth.frames.free_frames() == growth.frames.total_frames());
    CHECK(!owner.reserve(growth.pool, MappingPages::capacity + 1));
}

alignas(4096) std::array<std::byte, 4 * 4096> tight_backing{};
Arena tight;

void failed_growth_keeps_the_directory()
{
    tight.activate(tight_backing.data(), tight_backing.size() / page, 2);
    MappingPages owner;
    CHECK(owner.bind(tight.pool));
    for (usize index = 0; index < MappingPages::records_per_page; ++index)
        CHECK(owner.push_back({arch::execution::user_begin + index * page, 0x2000, 1}));
    CHECK(owner.size() == MappingPages::records_per_page && tight.pool.object_count() == 1);
    // Doubling needs two pages while the old run is held: the reserve refuses.
    CHECK(owner.push_back({arch::execution::user_end - page, 0x2000, 1}).code() == StatusCode::no_memory);
    CHECK(owner.size() == MappingPages::records_per_page && owner.record_pages() == 1 && tight.pool.object_count() == 1);
    CHECK(owner[MappingPages::records_per_page - 1].virtual_address ==
          arch::execution::user_begin + (MappingPages::records_per_page - 1) * page);
    CHECK(owner.clear() && tight.pool.object_count() == 0);
}

alignas(4096) std::array<std::byte, 6 * 4096> replace_backing{};
Arena replacing;

void replace_keeps_address_and_token_pairing()
{
    replacing.activate(replace_backing.data(), replace_backing.size() / page, 4);
    auto original = replacing.pool.allocate_frame();
    auto copy = replacing.pool.allocate_frame();
    CHECK(original && copy);
    MappingPages owner;
    CHECK(owner.reserve(replacing.pool, 1, true) && owner.frame_pages() == 1 && owner.record_pages() == 1);
    CHECK(owner.reserved() == MappingPages::frames_per_page);
    const auto address = arch::execution::user_begin;
    CHECK(!owner.push_back({address, original.value().address, arch::execution::page_read, 1}));
    CHECK(owner.push_back({address, original.value().address, arch::execution::page_read, 1}, original.value()));
    const auto *published = owner.view().records;
    // The address is fixed and the record must name its own token.
    CHECK(!owner.replace(0, {address + page, copy.value().address, 3}, copy.value()));
    CHECK(!owner.replace(0, {address, original.value().address, 3}, copy.value()));
    CHECK(!owner.replace(1, {address, copy.value().address, 3}, copy.value()));
    CHECK(owner[0].physical_address == original.value().address && owner[0].copy_on_write);
    CHECK(owner.replace(0, {address, copy.value().address, 3}, copy.value()));
    CHECK(&owner[0] == published && owner[0].flags == 3 && !owner[0].copy_on_write);
    CHECK(owner.frame(0)->address == copy.value().address && owner.frame(0)->generation == copy.value().generation);
    CHECK(owner.frame(owner[0]) == owner.frame(0) && !owner.frame(1));
    CHECK(replacing.pool.release_reference(original.value()));
    CHECK(!owner.replace(0, {address, original.value().address, 1}, original.value()));
    CHECK(owner.clear() && replacing.pool.release_reference(copy.value()));
    CHECK(replacing.pool.used_count() == 0);
}

alignas(4096) std::array<std::byte, 3 * 4096> failed_backing{};
Arena failed;

void failed_final_release_keeps_canonical_tokens()
{
    failed.activate(failed_backing.data(), failed_backing.size() / page, 2);
    {
        MappingPages owner;
        CHECK(owner.reserve(failed.pool, MappingPages::records_per_page + 1));
        CHECK(owner.record_pages() == 2 && failed.pool.object_count() == 2);
        ok::user::native::MappingPagesFaultProbe::overstate_record_run(owner);
        CHECK(!owner.clear() && owner.record_pages() == 3 && failed.pool.object_count() == 2);
        CHECK(failed.pool.quarantined_kernel_page_count() == 0);
    }
    // The destructor quarantines the frames the run really owns.
    CHECK(failed.pool.object_count() == 2 && failed.pool.quarantined_kernel_page_count() == 2);
    usize witnessed = 0;
    for (usize index = 0; index < failed.frames.total_frames(); ++index)
        if (const auto frame = failed.pool.quarantined_kernel_page(index))
        {
            CHECK(frame->owner == &failed.frames && frame->generation && frame->index == index);
            ++witnessed;
        }
    CHECK(witnessed == 2);
}
}

int main()
{
    growth_moves_unpublished_runs_only();
    failed_growth_keeps_the_directory();
    replace_keeps_address_and_token_pairing();
    failed_final_release_keeps_canonical_tokens();
    std::puts("PASS native mapping metadata: contiguous growth, publication pin, retirement bytes, fallible clone, in-place replacement, unique relocation and retained failed-release runs; hosted=1 guest=0");
}
