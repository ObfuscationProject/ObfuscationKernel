#pragma once

#include "ok/core/types.hpp"
#include <array>

namespace ok::user::native
{
struct PipeVector
{
    uptr base{};
    usize length{};
    [[nodiscard]] constexpr bool operator==(const PipeVector &) const = default;
};
// Value-owned metadata only. Payload mappings and bytes are checked on retry.
struct PipeVectors
{
    static constexpr usize capacity = 16;
    std::array<PipeVector, capacity> entries{};
    usize count{}; // Zero denotes a scalar continuation.
    [[nodiscard]] constexpr bool operator==(const PipeVectors &) const = default;
    [[nodiscard]] constexpr std::optional<usize> staged_count(usize limit) const
    {
        if (count > capacity) return std::nullopt;
        usize total = 0;
        for (usize i = 0; i < count; ++i)
        {
            if (entries[i].length > static_cast<usize>(__INTPTR_MAX__) - total) return std::nullopt;
            total += entries[i].length;
        }
        return total < limit ? total : limit;
    }
};
} // namespace ok::user::native
