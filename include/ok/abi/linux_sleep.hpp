#pragma once

#include "ok/abi/linux_user.hpp"

namespace ok::abi::linux_user
{
// Linux ktime_set saturates at the first whole second reaching KTIME_SEC_MAX.
// Input validation precedes this conversion at the syscall boundary.
inline constexpr u64 ktime_maximum = 0x7fffffffffffffffULL;
inline constexpr u64 linux_sleep_deadline(u64 now, Timespec request, bool absolute)
{
    constexpr u64 second = 1000000000;
    const u64 interval = static_cast<u64>(request.seconds) >= ktime_maximum / second
        ? ktime_maximum : static_cast<u64>(request.seconds) * second + static_cast<u64>(request.nanoseconds);
    if (absolute)
        return interval;
    return now >= ktime_maximum || interval > ktime_maximum - now ? ktime_maximum : now + interval;
}
} // namespace ok::abi::linux_user
