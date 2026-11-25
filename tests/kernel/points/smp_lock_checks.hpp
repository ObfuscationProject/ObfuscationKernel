#pragma once
#include "ok/smp/lock.hpp"

#include <type_traits>

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
// the lock it wants is free, so the check cannot deadlock. A same-rank pair
// taken with lock_as for the second is in order.
inline Status check_ranked_lock_order()
{
    smp::RankedSpinLock<smp::LockRank::task> outer;
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
    smp::RankedSpinLock<smp::LockRank::futex_bucket> lower;
    smp::RankedSpinLock<smp::LockRank::futex_bucket> higher;
    lower.lock();
    higher.lock_as<smp::LockRank::futex_bucket_second>();
    ordered = ordered && lower.owned_by_current_cpu() && higher.owned_by_current_cpu();
    higher.unlock();
    lower.unlock();
    ordered = ordered && detail::ranked_lock_reports == 0 && !outer.owned_by_current_cpu() &&
              !inner.owned_by_current_cpu() && !lower.owned_by_current_cpu() && !higher.owned_by_current_cpu();
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
            report.held != smp::LockRank::run_queue || report.wanted != smp::LockRank::task ||
            report.lock != &outer)
            return Status::fault("reverse lock order was not reported");
    }
    else if (reports != 0)
        return Status::fault("release lock checks reported a violation");
    if (!released)
        return Status::fault("reverse-order locks were not released");
    return Status::success();
}

// The booted kernel's fine locks: spin locks exactly on the profiles that
// start secondaries (xmake/arch.lua defines OK_ARCH_SMP_START for them), an
// UpLock elsewhere; either way a try on a held lock fails. A runtime check,
// so freestanding compile-only users of this header still build.
inline Status check_fine_lock_selection()
{
#if defined(OK_ARCH_TARGET_X86_64) || defined(OK_ARCH_TARGET_AARCH64) || defined(OK_ARCH_TARGET_RV64)
    constexpr bool smp_profile = true;
#else
    constexpr bool smp_profile = false;
#endif
    using Fine = smp::FineLock<smp::LockRank::task>;
    if (smp::concurrent_locks != smp_profile ||
        std::is_same_v<Fine, smp::RankedSpinLock<smp::LockRank::task>> != smp_profile)
        return Status::fault("fine locks do not match the profile's secondary-CPU start");
    Fine lock;
    lock.lock();
    const bool held = lock.owned_by_current_cpu() && !lock.try_lock();
    lock.unlock();
    if (!held || lock.owned_by_current_cpu())
        return Status::fault("a fine lock's try or owner answer is wrong");
    return Status::success();
}
} // namespace ok::tests
