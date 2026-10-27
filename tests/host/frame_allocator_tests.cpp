#include "../kernel/points/frame_allocator_checks.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

using namespace ok;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!static_cast<bool>(condition))                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                         \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

static_assert(!std::is_copy_constructible_v<memory::FrameAllocator>);
static_assert(!std::is_move_constructible_v<memory::FrameAllocator>);

void check_boundaries()
{
    memory::FrameAllocator frames;
    memory::FrameMetadataStorage<4, 4> metadata;
    CHECK(frames.total_frames() == 0 && frames.free_frames() == 0);
    CHECK(frames.allocate().status().code() == StatusCode::not_initialized);
    CHECK(!frames.release({}));
    CHECK(frames.initialize({}, 4096, metadata.bytes).code() == StatusCode::no_memory);
    const std::array fragments{
        memory::MemoryRegion{0, 1, memory::RegionType::usable},
        memory::MemoryRegion{1, 4095, memory::RegionType::usable},
        memory::MemoryRegion{0x3000, 0, memory::RegionType::reserved},
        memory::MemoryRegion{~uptr{0} - 8191, 8191, memory::RegionType::usable},
    };
    CHECK(frames.initialize(fragments, 4096, metadata.bytes));
    CHECK(frames.total_frames() == 2);
    auto zero = frames.allocate();
    auto high = frames.allocate();
    CHECK(zero && zero.value().address == 0);
    CHECK(high && high.value().address == ~uptr{0} - 8191);
    CHECK(frames.release(zero.value()));
    CHECK(frames.release(high.value()));
    const std::array partial{memory::MemoryRegion{1, 4095, memory::RegionType::usable}};
    CHECK(frames.initialize(partial, 4096, metadata.bytes).code() == StatusCode::no_memory);
    CHECK(frames.total_frames() == 2 && frames.free_frames() == 2);
    const std::array large_pages{memory::MemoryRegion{4096, 16384, memory::RegionType::usable}};
    CHECK(frames.initialize(large_pages, 8192, metadata.bytes));
    auto large = frames.allocate();
    CHECK(large && large.value().address == 8192 && frames.total_frames() == 1);
    CHECK(frames.release(large.value()));
}

void check_capacity_and_foreign_handles()
{
    // Metadata is sized from the map: a map beyond the old fixed table works.
    constexpr usize frame_count = 16384 + 3;
    memory::FrameAllocator frames;
    memory::FrameAllocator other;
    static memory::FrameMetadataStorage<frame_count> metadata;
    static memory::FrameMetadataStorage<frame_count> other_metadata;
    const std::array regions{memory::MemoryRegion{0x1000, frame_count * 4096, memory::RegionType::usable}};
    CHECK(memory::FrameAllocator::metadata_bytes(regions, 4096).value() <= metadata.bytes.size());
    CHECK(frames.initialize(regions, 4096, metadata.bytes));
    CHECK(other.initialize(regions, 4096, other_metadata.bytes));
    CHECK(frames.total_frames() == frame_count);
    static std::array<memory::PhysicalFrame, frame_count> owned{};
    for (usize i = 0; i < owned.size(); ++i)
    {
        auto frame = frames.allocate();
        CHECK(frame && frame.value().address == 0x1000 + i * 4096);
        owned[i] = frame.value();
    }
    CHECK(frames.allocate().status().code() == StatusCode::no_memory);
    auto foreign = other.allocate();
    CHECK(foreign && !frames.release(foreign.value()) && !other.release(owned[0]));
    CHECK(frames.free_frames() == 0 && other.free_frames() == frame_count - 1);
    CHECK(other.release(foreign.value()));
    for (auto frame : owned)
    {
        CHECK(frames.release(frame));
    }
    CHECK(frames.free_frames() == frame_count);
    // Many disjoint ranges need range metadata, not a fixed range table.
    std::array<memory::MemoryRegion, 65> fragmented{};
    for (usize i = 0; i < fragmented.size(); ++i)
    {
        fragmented[i] = {i * 8192, 4096, memory::RegionType::usable};
    }
    memory::FrameMetadataStorage<65, 65> fragmented_metadata;
    memory::FrameMetadataStorage<65, 1> short_metadata;
    CHECK(frames.initialize(fragmented, 4096, short_metadata.bytes).code() == StatusCode::no_memory);
    CHECK(frames.free_frames() == frame_count);
    CHECK(frames.initialize(fragmented, 4096, fragmented_metadata.bytes));
    CHECK(frames.total_frames() == 65 && frames.ranges().size() == 65);
    auto last = frames.handle_at(64);
    CHECK(!last);
    usize taken = 0;
    while (auto frame = frames.allocate())
    {
        CHECK(frame.value().address == taken * 8192);
        ++taken;
    }
    CHECK(taken == 65);
}

void check_map_order_and_reference_oracle()
{
    // Compare against a byte coverage oracle, including sub-page overlaps.
    constexpr usize page_size = 16;
    constexpr usize byte_count = 1024;
    u32 random = 0x27182818;
    auto next_random = [&] {
        random = random * 1664525u + 1013904223u;
        return random;
    };
    memory::FrameAllocator frames;
    memory::FrameMetadataStorage<byte_count / page_size, 12> metadata;
    for (usize trial = 0; trial < 200; ++trial)
    {
        std::array<memory::MemoryRegion, 12> regions{};
        std::array<bool, byte_count> usable{};
        std::array<bool, byte_count> reserved{};
        for (auto &region : regions)
        {
            region.base = next_random() % byte_count;
            region.size = next_random() % (byte_count - region.base + 1);
            region.type = next_random() % 4 == 0 ? memory::RegionType::reserved : memory::RegionType::usable;
            for (usize byte = region.base; byte < region.base + region.size; ++byte)
            {
                (region.type == memory::RegionType::usable ? usable : reserved)[byte] = true;
            }
        }
        std::array<uptr, byte_count / page_size> expected{};
        usize count = 0;
        for (usize page = 0; page < byte_count; page += page_size)
        {
            bool available = true;
            for (usize byte = page; byte < page + page_size; ++byte)
            {
                available &= usable[byte] && !reserved[byte];
            }
            if (available)
            {
                expected[count++] = page;
            }
        }
        for (usize order = 0; order < 2; ++order)
        {
            auto status = frames.initialize(regions, page_size, metadata.bytes);
            if (count == 0)
            {
                CHECK(status.code() == StatusCode::no_memory);
            }
            else
            {
                CHECK(status && frames.total_frames() == count);
                std::array<memory::PhysicalFrame, byte_count / page_size> owned{};
                for (usize i = 0; i < count; ++i)
                {
                    auto frame = frames.allocate();
                    CHECK(frame && frame.value().address == expected[i]);
                    owned[i] = frame.value();
                }
                CHECK(frames.allocate().status().code() == StatusCode::no_memory);
                for (usize i = 0; i < count; ++i)
                {
                    CHECK(frames.release(owned[i]));
                }
            }
            std::reverse(regions.begin(), regions.end());
        }
    }
}

int main()
{
    CHECK(test::check_frame_allocator());
    CHECK(test::check_frame_allocator());
    check_boundaries();
    check_capacity_and_foreign_handles();
    check_map_order_and_reference_oracle();
    std::puts("PASS: runtime-sized physical frame bookkeeping (hosted model)");
}
