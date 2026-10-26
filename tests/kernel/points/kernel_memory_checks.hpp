#pragma once

#include "ok/fs/page_cache.hpp"
#include "ok/memory/memory.hpp"

#include <array>

namespace ok::test
{

// Runs against the booted kernel's allocator over discovered RAM. Frames,
// objects and page-cache pages are written and read back through supervisor
// addresses, so this touches real QEMU guest memory; it does not qualify user
// mappings, DMA or any backing-store I/O.
inline Status check_kernel_memory(memory::MemoryManager &memory)
{
    auto &frames = memory.frames();
    const auto carve = memory.metadata_region();
    if (!carve.size || frames.index_of(carve.base) || frames.index_of(carve.base + carve.size - frames.page_size()))
        return Status::fault("frame metadata carve is absent or still managed");

    memory::ChargeAccount account;
    auto run = frames.allocate_contiguous(2, account);
    if (!run)
        return run.status();
    auto *bytes = reinterpret_cast<volatile u32 *>(run.value().address);
    const usize words = 2 * frames.page_size() / sizeof(u32);
    for (usize i = 0; i < words; ++i)
        bytes[i] = static_cast<u32>(0x5a5a0000u ^ i);
    for (usize i = 0; i < words; ++i)
        if (bytes[i] != static_cast<u32>(0x5a5a0000u ^ i))
            return Status::fault("discovered RAM did not retain a frame pattern");
    if (auto status = frames.release_contiguous(run.value(), 2, account); !status.ok())
        return status;

    auto &objects = memory.objects();
    if (!objects.initialized())
        return Status::not_initialized("kernel object allocator is not initialized");
    const auto before = objects.accounting();
    constexpr std::array<usize, 8> sizes{24, 40, 100, 250, 500, 1000, 3000, 9000};
    std::array<void *, sizes.size()> live{};
    memory::ChargeAccount owner;
    Status result{};
    usize count = 0;
    for (; count < sizes.size(); ++count)
    {
        auto object = objects.allocate(sizes[count], owner);
        if (!object)
        {
            result = object.status();
            break;
        }
        live[count] = object.value();
        auto *data = static_cast<u8 *>(live[count]);
        for (usize i = 0; i < sizes[count]; ++i)
        {
            if (data[i] != 0)
                result = Status::fault("kernel object was not zeroed");
            data[i] = static_cast<u8>(count * 31 + i);
        }
    }
    for (usize index = 0; index < count && result.ok(); ++index)
    {
        const auto *data = static_cast<const u8 *>(live[index]);
        for (usize i = 0; i < sizes[index]; ++i)
            if (data[i] != static_cast<u8>(index * 31 + i))
                result = Status::fault("kernel objects overlap or lost their contents");
    }
    for (usize index = 0; index < count; ++index)
        if (auto status = objects.release(live[index], sizes[index], owner); !status.ok() && result.ok())
            result = status;
    if (!result.ok())
        return result;
    const auto after = objects.accounting();
    for (usize index = 0; index < after.classes.size(); ++index)
        if (after.classes[index].live_objects != before.classes[index].live_objects)
            return Status::fault("kernel object release left live objects");
    if (owner.used() != 0 || after.large_pages != before.large_pages)
        return Status::fault("kernel object charges were not returned");

    // A node-owned page cache over the same frames and objects.
    memory::ChargeAccount cache_pages;
    fs::FilePageCache cache;
    if (auto status = cache.bind(memory, cache_pages); !status.ok())
        return status;
    for (u64 index : {3u, 0u, 12u})
    {
        auto page = cache.obtain(index);
        if (!page)
            return page.status();
        auto *data = reinterpret_cast<volatile u8 *>(page.value().address);
        data[0] = static_cast<u8>(index + 1);
        data[frames.page_size() - 1] = static_cast<u8>(index + 2);
        if (auto status = cache.mark_uptodate(index); !status.ok())
            return status;
    }
    if (auto status = cache.mark_dirty(12); !status.ok())
        return status;
    auto dirty = cache.next_dirty(0);
    const auto *first = reinterpret_cast<const volatile u8 *>(cache.lookup(3).value().address);
    if (!dirty || dirty.value() != 12 || first[0] != 4 || first[frames.page_size() - 1] != 5 || cache.clear().ok())
        return Status::fault("page cache lost its contents or dirty state");
    if (auto status = cache.mark_clean(12); !status.ok())
        return status;
    if (auto status = cache.clear(); !status.ok())
        return status;
    if (cache_pages.used() != 0)
        return Status::fault("page cache frames were not returned");
    return Status::success();
}

} // namespace ok::test
