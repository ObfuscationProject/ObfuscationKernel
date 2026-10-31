#pragma once
#include "ok/smp/lock.hpp"

namespace ok::tests
{
namespace detail
{
inline constinit u32 ranked_lock_reports = 0;
inline constinit smp::LockViolation ranked_lock_report{};

inline void record_ranked_lock_report(const smp::LockViolation &violation) noexcept
{
    ++ranked_lock_reports;
    ranked_lock_report = violation;
}
} // namespace detail

// Boot CPU only: a reverse-order lock() is reported before it would wait, and
// the lock it wants is free, so the check cannot deadlock.
inline Status check_ranked_lock_order()
{
    smp::RankedSpinLock<smp::LockRank::scheduler> outer;
    smp::RankedSpinLock<smp::LockRank::run_queue> inner;
    const auto previous = smp::lock_violation_handler;
    smp::lock_violation_handler = detail::record_ranked_lock_report;
    detail::ranked_lock_reports = 0;
    bool ordered = false;
    {
        smp::RankedSpinGuard outer_guard{outer};
        smp::RankedSpinGuard inner_guard{inner};
        ordered = outer.owned_by_current_cpu() && inner.owned_by_current_cpu();
    }
    ordered = ordered && detail::ranked_lock_reports == 0 && !outer.owned_by_current_cpu() &&
              !inner.owned_by_current_cpu();
    inner.lock();
    outer.lock();
    const auto reports = detail::ranked_lock_reports;
    const auto report = detail::ranked_lock_report;
    outer.unlock();
    inner.unlock();
    const bool released = !outer.owned_by_current_cpu() && !inner.owned_by_current_cpu() &&
                          detail::ranked_lock_reports == reports;
    smp::lock_violation_handler = previous;
    if (!ordered)
        return Status::fault("ranked locks taken in order were not owned and released cleanly");
    if constexpr (smp::lock_debug_enabled)
    {
        if (reports != 1 || report.kind != smp::LockViolationKind::order ||
            report.held != smp::LockRank::run_queue || report.wanted != smp::LockRank::scheduler ||
            report.lock != &outer)
            return Status::fault("reverse lock order was not reported");
    }
    else if (reports != 0)
        return Status::fault("release lock checks reported a violation");
    if (!released)
        return Status::fault("reverse-order locks were not released");
    return Status::success();
}
} // namespace ok::tests
