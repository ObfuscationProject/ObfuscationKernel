#include "ok/memory/physical_map.hpp"
#include "ok/memory/slab.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>

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

namespace
{

constexpr usize ram_pages = 64;

// Host-backed "RAM": slab pages are written through their frame addresses.
struct Fixture
{
    alignas(4096) std::array<std::byte, ram_pages * 4096> ram{};
    FrameMetadataStorage<ram_pages> metadata;
    FrameAllocator frames;
    SlabAllocator slab;
    Fixture(usize page_limit = 0, usize reserve = 0)
    {
        const MemoryRegion region{reinterpret_cast<uptr>(ram.data()), ram.size(), RegionType::usable};
        CHECK(frames.initialize({&region, 1}, 4096, metadata.bytes));
        CHECK(slab.initialize(frames, page_limit, reserve));
    }
};

bool zeroed(const void *object, usize size)
{
    const auto *bytes = static_cast<const std::byte *>(object);
    for (usize i = 0; i < size; ++i)
        if (bytes[i] != std::byte{})
            return false;
    return true;
}

void initialization()
{
    FrameAllocator unused;
    SlabAllocator slab;
    ChargeAccount owner;
    CHECK(slab.allocate(16, owner).status().code() == StatusCode::not_initialized);
    CHECK(slab.initialize(unused, 0, 0).code() == StatusCode::invalid_argument);
    alignas(8) std::array<std::byte, 256> metadata{};
    const MemoryRegion big{0x100000, 4 * 8192, RegionType::usable};
    CHECK(unused.initialize({&big, 1}, 8192, metadata));
    CHECK(slab.initialize(unused, 0, 0).code() == StatusCode::invalid_argument);
    static Fixture fixture;
    CHECK(fixture.slab.initialize(fixture.frames, 0, 0).code() == StatusCode::already_exists);
    SlabAllocator reserved;
    CHECK(reserved.initialize(fixture.frames, 0, ram_pages).code() == StatusCode::invalid_argument);
}

void size_classes()
{
    static Fixture fixture;
    auto &slab = fixture.slab;
    ChargeAccount owner;
    std::set<uptr> seen;
    usize charged = 0;
    for (usize size : {1, 16, 17, 33, 64, 100, 200, 300, 513, 1024})
    {
        auto object = slab.allocate(size, owner);
        CHECK(object);
        const auto address = reinterpret_cast<uptr>(object.value());
        const usize class_size = slab.charge_for(size);
        CHECK(class_size >= size && address % class_size == 0 && zeroed(object.value(), class_size));
        CHECK(seen.insert(address).second);
        charged += class_size;
        CHECK(owner.used() == charged);
        auto *bytes = static_cast<std::byte *>(object.value());
        for (usize i = 0; i < class_size; ++i)
            bytes[i] = std::byte{0xa5};
    }
    // Distinct classes never share a page; 16 and 17 bytes do not share a class.
    const auto accounting = slab.accounting();
    CHECK(accounting.pages == 7 && accounting.classes[0].live_objects == 2 && accounting.classes[1].live_objects == 1 &&
          accounting.classes[2].live_objects == 2 && accounting.classes[6].live_objects == 2);
    CHECK(accounting.classes[6].slots_per_page == 3 && accounting.classes[0].slots_per_page == 252);
    CHECK(accounting.allocations[static_cast<usize>(AllocationContext::kernel)] == 10);
    // A full class page opens a second one.
    std::array<void *, 3> big{};
    for (auto &object : big)
    {
        auto allocated = slab.allocate(1000, owner);
        CHECK(allocated);
        object = allocated.value();
    }
    CHECK(slab.accounting().classes[6].pages == 2 && slab.accounting().classes[6].live_objects == 5);
    for (auto *object : big)
        CHECK(slab.release(object, 1000, owner));
}

void release_checks()
{
    static Fixture fixture;
    auto &slab = fixture.slab;
    ChargeAccount owner;
    auto a = slab.allocate(64, owner);
    auto b = slab.allocate(64, owner);
    CHECK(a && b && owner.used() == 128);
    const auto before = slab.accounting();
    auto *bytes = static_cast<std::byte *>(a.value());
    CHECK(slab.release(a.value(), 128, owner).code() == StatusCode::invalid_argument);       // wrong class
    CHECK(slab.release(bytes + 8, 64, owner).code() == StatusCode::invalid_argument);       // not a slot start
    CHECK(slab.release(bytes - 64, 64, owner).code() == StatusCode::invalid_argument);      // header, not a slot
    CHECK(slab.release(nullptr, 64, owner).code() == StatusCode::invalid_argument);
    ChargeAccount stranger;
    CHECK(slab.release(a.value(), 64, stranger).code() == StatusCode::invalid_argument);   // no charge held
    static std::array<std::byte, 4096> outside{};
    CHECK(slab.release(outside.data(), 64, owner).code() == StatusCode::invalid_argument); // unmanaged memory
    ChargeAccount other;
    auto foreign = fixture.frames.allocate(other);
    CHECK(foreign && slab.release(reinterpret_cast<void *>(foreign.value().address + 64), 64, owner).code() ==
                         StatusCode::invalid_argument);                                     // another owner's frame
    const auto after = slab.accounting();
    CHECK(after.classes[2].live_objects == before.classes[2].live_objects && owner.used() == 128);
    CHECK(slab.release(a.value(), 64, owner) && owner.used() == 64);
    CHECK(slab.release(a.value(), 64, owner).code() == StatusCode::invalid_argument);      // double release
    CHECK(slab.release(b.value(), 64, owner) && owner.used() == 0);
    CHECK(slab.accounting().releases == 2);
}

void reclamation()
{
    static Fixture fixture;
    auto &slab = fixture.slab;
    ChargeAccount owner;
    std::array<void *, 21> objects{}; // Three 512-byte pages of seven slots.
    for (auto &object : objects)
    {
        auto allocated = slab.allocate(512, owner);
        CHECK(allocated);
        object = allocated.value();
    }
    const usize free_before = fixture.frames.free_frames();
    CHECK(slab.accounting().classes[5].pages == 3);
    for (auto *object : objects)
        CHECK(slab.release(object, 512, owner));
    // Empty pages return to the frame allocator; one stays cached for the class.
    CHECK(slab.accounting().classes[5].pages == 1 && slab.accounting().pages == 1);
    CHECK(fixture.frames.free_frames() == free_before + 2 && owner.used() == 0);
    auto again = slab.allocate(512, owner);
    CHECK(again && slab.accounting().pages == 1 && slab.release(again.value(), 512, owner));
}

void limits_and_reserve()
{
    {
        static Fixture fixture{2, 0};
        auto &slab = fixture.slab;
        ChargeAccount owner;
        CHECK(slab.allocate(16, owner) && slab.allocate(32, owner));
        CHECK(slab.allocate(64, owner).status().code() == StatusCode::no_memory);
        CHECK(owner.used() == 48 && slab.accounting().failures == 1);
        ChargeAccount tight{16};
        CHECK(slab.allocate(16, tight) && slab.allocate(16, tight).status().code() == StatusCode::no_memory);
        CHECK(tight.used() == 16);
    }
    {
        static Fixture fixture{0, 4};
        auto &slab = fixture.slab;
        ChargeAccount filler;
        while (fixture.frames.free_frames() > 5)
            CHECK(fixture.frames.allocate(filler));
        ChargeAccount owner;
        CHECK(slab.allocate(16, owner, AllocationContext::boot));     // Leaves exactly the reserve.
        CHECK(slab.allocate(32, owner, AllocationContext::kernel).status().code() == StatusCode::no_memory);
        CHECK(slab.allocate(4096, owner).status().code() == StatusCode::no_memory);
        auto atomic = slab.allocate(32, owner, AllocationContext::atomic);
        CHECK(atomic && fixture.frames.free_frames() == 3);
        // Existing partial pages serve every context without touching the reserve.
        CHECK(slab.allocate(16, owner, AllocationContext::kernel));
        CHECK(slab.accounting().allocations[static_cast<usize>(AllocationContext::atomic)] == 1);
    }
}

void large_objects()
{
    static Fixture fixture;
    auto &slab = fixture.slab;
    ChargeAccount owner;
    auto object = slab.allocate(5000, owner);
    CHECK(object && reinterpret_cast<uptr>(object.value()) % 4096 == 0 && owner.used() == 8192);
    CHECK(zeroed(object.value(), 8192) && slab.accounting().large_pages == 2 && slab.accounting().pages == 2);
    CHECK(slab.release(object.value(), 4096 * 3, owner).code() == StatusCode::invalid_argument);
    CHECK(slab.release(static_cast<std::byte *>(object.value()) + 4096, 5000, owner).code() ==
          StatusCode::invalid_argument);
    const usize free_before = fixture.frames.free_frames();
    CHECK(slab.release(object.value(), 8000, owner) && owner.used() == 0);
    CHECK(fixture.frames.free_frames() == free_before + 2 && slab.accounting().large_allocations == 0);
    CHECK(slab.release(object.value(), 8000, owner).code() == StatusCode::invalid_argument);
    CHECK(slab.allocate(ram_pages * 4096 + 1, owner).status().code() == StatusCode::no_memory && owner.used() == 0);
}

void memory_manager_policy()
{
    MemoryManager model;
    FrameMetadataStorage<16> metadata;
    const MemoryRegion region{0x100000, 16 * 4096, RegionType::usable};
    CHECK(model.initialize({&region, 1}, 4096, metadata.bytes));
    CHECK(model.initialize_objects().code() == StatusCode::not_initialized && !model.objects().initialized());

    alignas(4096) static std::array<std::byte, 512 * 4096> ram{};
    PhysicalMemoryMap map;
    CHECK(map.add({reinterpret_cast<uptr>(ram.data()), ram.size(), RegionType::usable}, MapSource::fdt).ok());
    CHECK(map.normalize(4096).ok());
    static MemoryManager memory;
    CHECK(memory.initialize(map, 4096) && memory.initialize_objects());
    const usize total = memory.frames().total_frames();
    CHECK(memory.objects().initialized() && memory.objects().reserve_frames() == 16);
    CHECK(memory.objects().page_account().limit() == total / 4);
    ChargeAccount owner;
    auto object = memory.objects().allocate(200, owner);
    CHECK(object && memory.objects().release(object.value(), 200, owner));
    CHECK(memory.initialize_objects().code() == StatusCode::already_exists);
}

} // namespace

int main()
{
    initialization();
    size_classes();
    release_checks();
    reclamation();
    limits_and_reserve();
    large_objects();
    memory_manager_policy();
    std::puts("OK_HOST slab classes=pass release=pass reclaim=pass limits=pass reserve=pass large=pass policy=pass");
    return 0;
}
