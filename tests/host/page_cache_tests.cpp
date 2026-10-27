#include "ok/fs/page_cache.hpp"
#include "ok/memory/physical_map.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::memory;
using ok::fs::FilePageCache;

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

// Host-backed RAM through the boot form, so cached pages are writable bytes.
MemoryManager &memory()
{
    alignas(4096) static std::array<std::byte, 256 * 4096> ram{};
    static MemoryManager manager;
    static bool ready = false;
    if (!ready)
    {
        PhysicalMemoryMap map;
        CHECK(map.add({reinterpret_cast<uptr>(ram.data()), ram.size(), RegionType::usable}, MapSource::fdt).ok());
        CHECK(map.normalize(4096).ok());
        CHECK(manager.initialize(map, 4096) && manager.initialize_objects());
        ready = true;
    }
    return manager;
}

void binding()
{
    FilePageCache cache;
    ChargeAccount pages;
    CHECK(!cache.bound() && cache.lookup(0).status().code() == StatusCode::not_initialized);
    CHECK(cache.obtain(0).status().code() == StatusCode::not_initialized && cache.clear());
    MemoryManager model;
    FrameMetadataStorage<4> metadata;
    const MemoryRegion region{0x100000, 4 * 4096, RegionType::usable};
    CHECK(model.initialize({&region, 1}, 4096, metadata.bytes));
    CHECK(cache.bind(model, pages).code() == StatusCode::not_initialized);
    CHECK(cache.bind(memory(), pages) && cache.bind(memory(), pages).code() == StatusCode::already_exists);
}

void lifecycle()
{
    FilePageCache cache;
    ChargeAccount pages;
    CHECK(cache.bind(memory(), pages));
    const usize free_before = memory().frames().free_frames();
    const usize slab_before = memory().objects().accounting().pages;
    // Out-of-order creation keeps the index sorted; pages start zeroed and stale.
    for (u64 index : {7u, 2u, 40u, 0u, 9u, 3u, 100u, 5u, 6u, 8u})
    {
        auto page = cache.obtain(index);
        CHECK(page);
        const auto *bytes = reinterpret_cast<const std::byte *>(page.value().address);
        for (usize i = 0; i < 4096; ++i)
            CHECK(bytes[i] == std::byte{});
    }
    CHECK(pages.used() == 10 && memory().frames().free_frames() < free_before - 10 + 1);
    CHECK(cache.accounting().pages == 10 && cache.accounting().index_capacity == 16);
    auto again = cache.obtain(40);
    CHECK(again && again.value().address == cache.lookup(40).value().address && pages.used() == 10);
    CHECK(cache.lookup(41).status().code() == StatusCode::not_found);

    // Fill, mark dirty, walk write-back in index order, clean.
    auto *data = reinterpret_cast<std::byte *>(cache.lookup(9).value().address);
    data[0] = std::byte{0x42};
    CHECK(cache.mark_dirty(9).code() == StatusCode::invalid_argument);
    for (u64 index : {9u, 2u, 100u})
        CHECK(cache.mark_uptodate(index) && cache.mark_dirty(index));
    CHECK(cache.next_dirty(0).value() == 2 && cache.next_dirty(3).value() == 9 && cache.next_dirty(10).value() == 100);
    CHECK(cache.mark_clean(2) && cache.next_dirty(0).value() == 9);
    CHECK(cache.accounting().dirty == 2 && cache.accounting().uptodate == 3);

    // Eviction keeps pinned and dirty pages; truncate respects pins.
    CHECK(cache.pin(3) && cache.pin(3) && cache.accounting().pinned == 1);
    auto evicted = cache.evict_clean();
    CHECK(evicted && evicted.value() == 7 && cache.accounting().pages == 3);
    CHECK(cache.lookup(9) && cache.lookup(100) && cache.lookup(3) && cache.lookup(40).status().code() == StatusCode::not_found);
    CHECK(cache.lookup(9) && reinterpret_cast<std::byte *>(cache.lookup(9).value().address)[0] == std::byte{0x42});
    CHECK(cache.truncate(3).code() == StatusCode::busy && cache.accounting().pages == 3);
    CHECK(cache.truncate(10) && cache.accounting().pages == 2 && cache.next_dirty(10).status().code() == StatusCode::not_found);
    CHECK(cache.clear().code() == StatusCode::busy); // Pinned and dirty pages remain.
    CHECK(cache.unpin(3) && cache.unpin(3) && cache.unpin(3).code() == StatusCode::invalid_argument);
    CHECK(cache.clear().code() == StatusCode::busy); // Page 9 is still dirty.
    CHECK(cache.mark_clean(9) && cache.clear());
    CHECK(pages.used() == 0 && cache.accounting().pages == 0 && cache.accounting().index_capacity == 0);
    // Only the slab's cached class pages (its documented reclamation policy) remain.
    const usize slab_after = memory().objects().accounting().pages;
    CHECK(memory().frames().free_frames() + (slab_after - slab_before) == free_before);
}

void limits()
{
    FilePageCache cache;
    ChargeAccount pages{2};
    CHECK(cache.bind(memory(), pages));
    CHECK(cache.obtain(0) && cache.obtain(1));
    CHECK(cache.obtain(2).status().code() == StatusCode::no_memory && cache.accounting().pages == 2);
    CHECK(cache.pin(9).code() == StatusCode::not_found && cache.mark_uptodate(9).code() == StatusCode::not_found);
}

} // namespace

int main()
{
    binding();
    lifecycle();
    limits();
    std::puts("OK_HOST page_cache index=pass dirty=pass pins=pass evict=pass truncate=pass clear=pass limits=pass");
    return 0;
}
