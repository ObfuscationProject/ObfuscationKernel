#include "ok/memory/runtime_frames.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstddef>

using namespace ok;
using namespace ok::memory;

#define CHECK(condition)                                                                                   \
    do                                                                                                     \
    {                                                                                                      \
        if (!static_cast<bool>(condition))                                                                 \
        {                                                                                                  \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                            \
            std::abort();                                                                                  \
        }                                                                                                  \
    } while (false)

static void check_map_normalisation()
{
    PhysicalMemoryMap map;
    CHECK(map.add({0x100000, 0x200000, RegionType::usable}, MapSource::e820).ok());
    CHECK(map.add({0x180000, 0x1000, RegionType::reserved}, MapSource::e820).ok());
    CHECK(map.add({0x2FF000, 0x2000, RegionType::usable}, MapSource::e820).ok()); // overlaps and extends
    CHECK(map.reserve(0x100000, 0x10000, RegionType::kernel_image).ok());
    CHECK(map.add({0, 0, RegionType::usable}, MapSource::e820).ok()); // ignored
    CHECK(map.add({~uptr{0} - 10, 100, RegionType::usable}, MapSource::e820).code() == StatusCode::invalid_argument);
    CHECK(!map.normalized());
    CHECK(map.normalize(4096).ok());
    CHECK(map.normalized());
    const auto usable = map.usable();
    CHECK(usable.size() == 2);
    CHECK(usable[0].base == 0x110000 && usable[0].size == 0x70000);
    CHECK(usable[1].base == 0x181000 && usable[1].size == 0x180000);
    CHECK(map.usable_pages(4096) == (0x70000 + 0x180000) / 4096);
    CHECK(map.product_grade());

    PhysicalMemoryMap synthetic;
    CHECK(synthetic.add({0x100000, 64u << 20, RegionType::usable}, MapSource::synthetic).ok());
    CHECK(synthetic.normalize(4096).ok());
    CHECK(!synthetic.product_grade());
}

static void check_runtime_allocation()
{
    PhysicalMemoryMap map;
    CHECK(map.add({0x10000, 100 * 4096, RegionType::usable}, MapSource::fdt).ok());
    CHECK(map.add({0x100000, 28 * 4096, RegionType::usable}, MapSource::fdt).ok());
    CHECK(map.normalize(4096).ok());
    const usize need = RuntimeFrameAllocator::metadata_bytes_required(map, 4096);
    CHECK(need > 0 && need < 4096);
    alignas(8) std::array<std::byte, 4096> storage{};
    RuntimeFrameAllocator frames;
    ChargeAccount early;
    CHECK(frames.allocate(early).status().code() == StatusCode::not_initialized);
    CHECK(frames.initialize(map, 4096, std::span<std::byte>{storage.data(), need - 1}).code() == StatusCode::no_memory);
    CHECK(frames.initialize(map, 4096, storage).ok());
    CHECK(frames.total_frames() == 128 && frames.free_frames() == 128);
    CHECK(frames.initialize(map, 4096, storage).code() == StatusCode::busy);

    ChargeAccount quota{3};
    auto a = frames.allocate(quota);
    auto b = frames.allocate(quota);
    auto c = frames.allocate(quota);
    CHECK(a && b && c);
    CHECK(a.value().address == 0x10000 && b.value().address == 0x11000);
    CHECK(frames.allocate(quota).status().code() == StatusCode::no_memory); // quota, not exhaustion
    CHECK(quota.used() == 3 && quota.peak() == 3 && frames.free_frames() == 125);
    CHECK(frames.release(b.value(), quota).ok());
    CHECK(frames.release(b.value(), quota).code() == StatusCode::invalid_argument); // stale
    RuntimeFrame forged = a.value();
    forged.owner ^= 1;
    CHECK(frames.release(forged, quota).code() == StatusCode::invalid_argument);
    auto d = frames.allocate(quota);
    CHECK(d && d.value().index == 1 && d.value().generation != b.value().generation);

    ChargeAccount unlimited;
    usize taken = 0;
    while (frames.allocate(unlimited))
        ++taken;
    CHECK(taken == 125 && frames.free_frames() == 0);
    CHECK(frames.allocate(unlimited).status().code() == StatusCode::no_memory);
    CHECK(unlimited.used() == 125);
    // Second range addresses are reachable.
    CHECK(frames.release(c.value(), quota).ok());
    auto e = frames.allocate(unlimited);
    CHECK(e && e.value().index == 2);
}

int main()
{
    check_map_normalisation();
    check_runtime_allocation();
    std::puts("OK_HOST runtime_frames model=pass");
    return 0;
}
