#include "ok/memory/physical_map.hpp"

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

static void check_charged_allocation()
{
    PhysicalMemoryMap map;
    CHECK(map.add({0x10000, 100 * 4096, RegionType::usable}, MapSource::fdt).ok());
    CHECK(map.add({0x100000, 28 * 4096, RegionType::usable}, MapSource::fdt).ok());
    CHECK(map.normalize(4096).ok());
    auto need = FrameAllocator::metadata_bytes(map.usable(), 4096);
    CHECK(need && need.value() > 0 && need.value() < 4096);
    alignas(8) std::array<std::byte, 4096> storage{};
    FrameAllocator frames;
    ChargeAccount early;
    CHECK(frames.allocate(early).status().code() == StatusCode::not_initialized);
    CHECK(frames.initialize(map.usable(), 4096, std::span<std::byte>{storage.data(), need.value() - 1}).code() ==
          StatusCode::no_memory);
    CHECK(!frames.initialized());
    CHECK(frames.initialize(map.usable(), 4096, storage).ok());
    CHECK(frames.total_frames() == 128 && frames.free_frames() == 128 && frames.ranges().size() == 2);

    ChargeAccount quota{3};
    auto a = frames.allocate(quota);
    auto b = frames.allocate(quota);
    auto c = frames.allocate(quota);
    CHECK(a && b && c);
    CHECK(frames.initialize(map.usable(), 4096, storage).code() == StatusCode::busy);
    CHECK(a.value().address == 0x10000 && b.value().address == 0x11000);
    CHECK(frames.allocate(quota).status().code() == StatusCode::no_memory); // quota, not exhaustion
    CHECK(quota.used() == 3 && quota.peak() == 3 && frames.free_frames() == 125);
    CHECK(frames.release(b.value(), quota).ok());
    CHECK(frames.release(b.value(), quota).code() == StatusCode::invalid_argument); // stale
    PhysicalFrame forged = a.value();
    forged.owner = nullptr;
    CHECK(frames.release(forged, quota).code() == StatusCode::invalid_argument);
    ChargeAccount empty;
    CHECK(frames.release(a.value(), empty).code() == StatusCode::invalid_argument && frames.live(a.value()));
    auto d = frames.allocate(quota);
    CHECK(d && d.value().index == 1 && d.value().generation != b.value().generation);

    auto *descriptor = frames.descriptor(d.value());
    CHECK(descriptor && descriptor->generation == d.value().generation && descriptor->references == 0);
    descriptor->references = 7;
    CHECK(frames.index_of(d.value().address).value() == d.value().index);
    CHECK(frames.index_of(0x100000 + 27 * 4096).value() == 127);
    CHECK(frames.index_of(0x10001).status().code() == StatusCode::invalid_argument);
    CHECK(frames.index_of(0x10000 + 100 * 4096).status().code() == StatusCode::not_found);
    CHECK(frames.handle_at(d.value().index).value().generation == d.value().generation);
    CHECK(!frames.handle_at(50));

    ChargeAccount unlimited;
    usize taken = 0;
    while (frames.allocate(unlimited))
        ++taken;
    CHECK(taken == 125 && frames.free_frames() == 0);
    CHECK(frames.allocate(unlimited).status().code() == StatusCode::no_memory);
    CHECK(unlimited.used() == 125);
    // A released frame comes back with cleared owner fields.
    CHECK(frames.release(d.value(), quota).ok());
    auto e = frames.allocate(unlimited);
    CHECK(e && e.value().index == 1 && frames.descriptor(e.value())->references == 0);
}

static void check_contiguous()
{
    alignas(8) std::array<std::byte, 1024> storage{};
    FrameAllocator frames;
    const std::array regions{MemoryRegion{0x1000, 4 * 4096, RegionType::usable},
                             MemoryRegion{0x10000, 8 * 4096, RegionType::usable}};
    CHECK(frames.initialize(regions, 4096, storage));
    ChargeAccount account;
    auto single = frames.allocate(account);
    CHECK(single && single.value().address == 0x1000);
    auto run = frames.allocate_contiguous(6, account);
    CHECK(run && run.value().address == 0x10000 && account.used() == 7);
    CHECK(frames.handle_at(run.value().index + 5).value().address == 0x15000);
    CHECK(frames.allocate_contiguous(4, account).status().code() == StatusCode::no_memory && account.used() == 7);
    CHECK(frames.allocate_contiguous(0, account).status().code() == StatusCode::invalid_argument);
    auto pair = frames.allocate_contiguous(2, account);
    CHECK(pair && pair.value().address == 0x2000 && frames.handle_at(pair.value().index + 1).value().address == 0x3000);
    CHECK(account.used() == 9);
    ChargeAccount tight{1};
    CHECK(frames.allocate_contiguous(2, tight).status().code() == StatusCode::no_memory && tight.used() == 0);
    // A run releases as a whole; a partial or stale run is refused unchanged.
    ChargeAccount empty;
    CHECK(frames.release_contiguous(run.value(), 6, empty).code() == StatusCode::invalid_argument);
    CHECK(frames.release_contiguous(run.value(), 9, account).code() == StatusCode::invalid_argument);
    CHECK(frames.release(frames.handle_at(run.value().index + 2).value(), account));
    CHECK(frames.release_contiguous(run.value(), 6, account).code() == StatusCode::invalid_argument);
    CHECK(frames.release_contiguous(run.value(), 2, account) && account.used() == 6 && !frames.live(run.value()));
    CHECK(frames.release_contiguous(run.value(), 2, account).code() == StatusCode::invalid_argument);
    CHECK(frames.release_contiguous(frames.handle_at(run.value().index + 3).value(), 3, account) && account.used() == 3);
}

static void check_boot_carve()
{
    // Host-backed "RAM": the boot form writes its metadata into the carve.
    alignas(4096) static std::array<std::byte, 96 * 4096> low{};
    alignas(4096) static std::array<std::byte, 32 * 4096> high{};
    PhysicalMemoryMap map;
    const auto low_base = reinterpret_cast<uptr>(low.data());
    const auto high_base = reinterpret_cast<uptr>(high.data());
    CHECK(map.add({low_base, low.size(), RegionType::usable}, MapSource::fdt).ok());
    CHECK(map.add({high_base, high.size(), RegionType::usable}, MapSource::fdt).ok());
    CHECK(map.reserve(low_base, 4096, RegionType::kernel_image).ok());
    CHECK(map.normalize(4096).ok());
    MemoryManager memory;
    CHECK(memory.initialize(map, 4096));
    const auto carve = memory.metadata_region();
    const auto top = low_base > high_base ? low_base + low.size() : high_base + high.size();
    CHECK(carve.size == 4096 && carve.base + carve.size == top);
    CHECK(memory.frames().total_frames() == 96 + 32 - 1 - 1);
    usize count = 0;
    while (auto frame = memory.frames().allocate())
    {
        const auto address = frame.value().address;
        CHECK(address + 4096 <= carve.base || address >= carve.base + carve.size);
        CHECK(address != low_base);
        ++count;
    }
    CHECK(count == memory.frames().total_frames());
    MemoryManager offset;
    PhysicalMemoryMap tiny;
    CHECK(tiny.add({0x1000, 4096, RegionType::usable}, MapSource::fdt).ok());
    CHECK(tiny.normalize(4096).ok());
    CHECK(offset.initialize(tiny, 4096).code() == StatusCode::no_memory);
    PhysicalMemoryMap raw;
    CHECK(offset.initialize(raw, 4096).code() == StatusCode::not_initialized);
}

int main()
{
    check_map_normalisation();
    check_charged_allocation();
    check_contiguous();
    check_boot_carve();
    std::puts("OK_HOST physical_map model=pass");
    return 0;
}
