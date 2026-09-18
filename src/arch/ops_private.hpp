#pragma once

#include "ok/arch/native.hpp"

#include <atomic>

namespace ok::arch::detail
{

inline u64 fallback_cycle_counter() noexcept
{
    static std::atomic<u64> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

inline void compiler_fence() noexcept
{
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

} // namespace ok::arch::detail
