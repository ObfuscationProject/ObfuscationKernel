#pragma once

#include "ok/arch/execution.hpp"

#include <array>

// Translation-table storage for the standalone execution probes. In the kernel
// the native page pool supplies these runs (src/user/runtime/vm.cpp); a probe
// links only the backend, so it serves zero-filled, aligned page runs from a
// static arena with one occupancy flag per page. Include it from exactly one
// translation unit of a probe.
namespace ok::arch::execution::probe
{
inline constexpr usize table_arena_pages = 64;
// The arm32 first-level table is the largest alignment a backend requests.
alignas(16384) inline std::array<std::byte, table_arena_pages * page_size> table_arena{};
inline std::array<bool, table_arena_pages> table_used{};
inline usize table_pages_used{};
// A probe lowers the budget to make table growth fail at a chosen point.
inline usize table_page_budget = table_arena_pages;
} // namespace ok::arch::execution::probe

ok::Result<ok::uptr> ok::arch::execution::allocate_table_pages(usize pages, usize alignment)
{
    using namespace probe;
    if (!pages || pages > table_arena_pages || (alignment & (alignment - 1)))
        return Status::invalid_argument("probe table run is empty, oversized or misaligned");
    if (pages > table_page_budget - table_pages_used)
        return Status::no_memory("probe table budget exhausted");
    const auto base = reinterpret_cast<uptr>(table_arena.data());
    for (usize first = 0; first + pages <= table_arena_pages; ++first)
    {
        const uptr address = base + first * page_size;
        if (alignment && address % alignment)
            continue;
        bool free = true;
        for (usize index = 0; index < pages && free; ++index)
            free = !table_used[first + index];
        if (!free)
            continue;
        for (usize index = 0; index < pages; ++index)
            table_used[first + index] = true;
        table_pages_used += pages;
        // Volatile stores keep the compiler from emitting a memset call.
        auto *bytes = reinterpret_cast<volatile std::byte *>(address);
        for (usize offset = 0; offset < pages * page_size; ++offset)
            bytes[offset] = std::byte{};
        return address;
    }
    return Status::no_memory("probe table arena exhausted");
}

ok::Status ok::arch::execution::release_table_pages(uptr address, usize pages)
{
    using namespace probe;
    const auto base = reinterpret_cast<uptr>(table_arena.data());
    if (address < base || (address - base) % page_size || !pages ||
        (address - base) / page_size + pages > table_arena_pages)
        return Status::invalid_argument("probe table run is foreign");
    const usize first = (address - base) / page_size;
    for (usize index = 0; index < pages; ++index)
        if (!table_used[first + index])
            return Status::invalid_argument("probe table run is not wholly allocated");
    for (usize index = 0; index < pages; ++index)
        table_used[first + index] = false;
    table_pages_used -= pages;
    return Status::success();
}
