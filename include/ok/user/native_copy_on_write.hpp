#pragma once

#include "ok/user/native_mapping_pages.hpp"

namespace ok::user::native
{
// Copy-on-write sharing of private frames between a forked parent and child.
// A record keeps write access it is entitled to in `copy_on_write` while its
// `flags`, which every backend translates, withhold page_write. Shared
// anonymous and shared-file frames are coherent by design and never withheld.
// The xmake option native_cow_fork selects this; otherwise fork copies every
// private page eagerly and no record is ever withheld.
#ifdef OK_NATIVE_COW_FORK
inline constexpr bool copy_on_write_fork = true;
#else
inline constexpr bool copy_on_write_fork = false;
#endif

// Boot-lifetime counters for the serialized runtime's diagnostics line.
struct CopyOnWriteStats
{
    u64 shared{}, copied{}, reused{};
};
inline constinit CopyOnWriteStats copy_on_write_stats{};

[[nodiscard]] constexpr u8 intended_access(const arch::execution::PageMapping &page)
{
    return static_cast<u8>(page.flags | (page.copy_on_write ? arch::execution::page_write : 0));
}

// Without copy-on-write fork every private frame has exactly one reference, so
// this reduces to plain assignment of `access`.
inline void settle_access(arch::execution::PageMapping &page, u8 access, bool exclusive)
{
    const bool withheld = !exclusive && (access & arch::execution::page_write);
    page.flags = withheld ? static_cast<u8>(access & ~arch::execution::page_write) : access;
    page.copy_on_write = withheld;
}

[[nodiscard]] inline bool exclusive_frame(const NativePagePool &pool, uptr address)
{
    return pool.is_shared(address) || pool.reference_count(address) < 2;
}

// Fork: the child receives a private record with write withheld and one more
// reference on the parent's frame. Nothing is copied here.
inline Status share_private_page(MappingPages &child, const arch::execution::PageMapping &page,
                                 const memory::PhysicalFrame &frame, NativePagePool &pool)
{
    if (auto status = pool.retain_copy(frame); !status) return status;
    auto shared = page;
    settle_access(shared, intended_access(page), false);
    auto status = child.push_back(shared, frame);
    if (!status) static_cast<void>(pool.release_reference(frame));
    else ++copy_on_write_stats.shared;
    return status;
}

// After the child space exists, withhold write access from every parent record
// whose private frame is now shared. A failed remap leaves a writable
// translation of a shared frame, so the caller must stop the runtime.
inline Status withhold_shared_writes(MappingPages &pages, const arch::execution::AddressSpace &space,
                                     const NativePagePool &pool)
{
    for (usize index = 0; index < pages.size(); ++index)
    {
        auto &page = pages[index];
        if (!(page.flags & arch::execution::page_write) || exclusive_frame(pool, page.physical_address)) continue;
        settle_access(page, page.flags, false);
        if (auto status = arch::execution::remap(space, page); !status) return status;
    }
    return Status::success();
}

// Resolves a write to a withheld page of the live `space` that `pages`
// describes. A sole remaining owner regains write access in place; otherwise
// the record moves to a private copy. Returns false for any other page.
inline Result<bool> resolve_copy_on_write(MappingPages &pages, const arch::execution::AddressSpace &space,
                                          uptr address, NativePagePool &pool)
{
    const auto page_address = address & ~(arch::execution::page_size - 1);
    for (usize index = 0; index < pages.size(); ++index)
    {
        auto page = pages[index];
        if (page.virtual_address != page_address) continue;
        if (!page.copy_on_write || !page.flags) return false;
        const auto *owner = pages.frame(index);
        if (!owner || owner->address != page.physical_address)
            return Status::fault("copy-on-write page lost its exact frame token");
        const auto previous = *owner;
        const bool exclusive = exclusive_frame(pool, previous.address);
        auto frame = previous;
        if (!exclusive)
        {
            auto copied = pool.allocate_frame();
            if (!copied) return copied.status();
            frame = copied.value();
            auto *destination = reinterpret_cast<uptr *>(frame.address);
            const auto *source = reinterpret_cast<const uptr *>(previous.address);
            for (usize word = 0; word < arch::execution::page_size / sizeof(uptr); ++word)
                destination[word] = source[word];
            page.physical_address = frame.address;
        }
        settle_access(page, intended_access(page), true);
        if (auto status = pages.replace(index, page, frame); !status)
        {
            if (!exclusive) static_cast<void>(pool.release_reference(frame));
            return status;
        }
        // The record no longer names `previous`, so its reference goes even if
        // the remap fails; another owner still holds that frame.
        const auto mapped = arch::execution::remap(space, pages[index]);
        if (!exclusive)
            if (auto status = pool.release_reference(previous); !status) return status;
        if (!mapped) return mapped;
        ++(exclusive ? copy_on_write_stats.reused : copy_on_write_stats.copied);
        return true;
    }
    return false;
}

// Resolves a withheld page of whichever process owns the live `space`.
template <class Processes>
Result<bool> resolve_owner_write(Processes &&processes, const arch::execution::AddressSpace &space, uptr address,
                                 NativePagePool &pool)
{
    for (auto &process : processes)
    {
        const auto &live = process.resources().native_space;
        if (live.root == space.root && live.slot == space.slot && live.generation == space.generation)
            return resolve_copy_on_write(process.resources().native_pages, space, address, pool);
    }
    return false;
}
} // namespace ok::user::native
