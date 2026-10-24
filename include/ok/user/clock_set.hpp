#pragma once

#include "ok/time/epoch.hpp"
#include "ok/user/user.hpp"

namespace ok::user
{

struct ClockSetValue
{
    i64 seconds{};
    i64 nanoseconds{};
};

// Linux6.13.4 TIME_SETTOD_SEC_MAX: KTIME_SEC_MAX minus30 years of uptime.
inline constexpr i64 maximum_set_realtime_seconds = 8277292036LL;

// Callers finish clock-ID admission and input copying before this helper, and
// obtain credentials from the current kernel-owned process. No userspace ABI
// exposes either the credential record or the sampled monotonic anchor.
template <typename SampleMonotonic, typename CommitRealtime>
Status set_realtime_checked(ClockSetValue value, Credentials credentials,
                           SampleMonotonic &&sample, CommitRealtime &&commit)
{
    if (value.seconds < 0 || value.seconds >= maximum_set_realtime_seconds ||
        value.nanoseconds < 0 || value.nanoseconds >= static_cast<i64>(time::nanoseconds_per_second))
        return Status::invalid_argument("realtime setting exceeds supported timespec range");
    if (!is_privileged(credentials))
        return Status::permission("realtime setting requires administrative authority");
    auto point = sample();
    if (!point)
        return Status::io_error("realtime setting requires a continuous monotonic clock");
    const auto seconds = static_cast<i64>(point.value().nanoseconds / time::nanoseconds_per_second);
    const auto fraction = point.value().nanoseconds % time::nanoseconds_per_second;
    if (value.seconds < seconds ||
        (value.seconds == seconds && static_cast<u64>(value.nanoseconds) < fraction))
        return Status::invalid_argument("realtime setting precedes monotonic elapsed time");
    return commit(time::EpochTime{value.seconds, static_cast<u32>(value.nanoseconds)}, point.value());
}

} // namespace ok::user
