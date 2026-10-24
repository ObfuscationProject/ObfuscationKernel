#pragma once

#include "ok/core/types.hpp"
#include <array>

namespace ok::sched
{

struct SignalMask
{
    std::array<u64, 2> words{};
    constexpr bool contains(u32 signal) const
    {
        return signal >= 1 && signal <= 128 && (words[(signal - 1) / 64] & (u64{1} << ((signal - 1) % 64)));
    }
    constexpr void set(u32 signal, bool blocked = true)
    {
        if (signal < 1 || signal > 128) return;
        const u64 bit = u64{1} << ((signal - 1) % 64);
        if (blocked) words[(signal - 1) / 64] |= bit;
        else words[(signal - 1) / 64] &= ~bit;
    }
    constexpr bool operator==(const SignalMask &) const = default;
};


} // namespace ok::sched
