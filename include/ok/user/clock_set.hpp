#pragma once

#include "ok/time/epoch.hpp"
#include "ok/time/realtime_clock.hpp"
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

// Linux range and authority, checked before any clock sample.
inline Status check_realtime_setting(ClockSetValue value, Credentials credentials)
{
    if (value.seconds < 0 || value.seconds >= maximum_set_realtime_seconds ||
        value.nanoseconds < 0 || value.nanoseconds >= static_cast<i64>(time::nanoseconds_per_second))
        return Status::invalid_argument("realtime setting exceeds supported timespec range");
    if (!is_privileged(credentials))
        return Status::permission("realtime setting requires administrative authority");
    return Status::success();
}

// Callers finish clock-ID admission and input copying before this helper, and
// obtain credentials from the current kernel-owned process. No userspace ABI
// exposes either the credential record or the sampled monotonic anchor.
template <typename SampleMonotonic, typename CommitRealtime>
Status set_realtime_checked(ClockSetValue value, Credentials credentials,
                           SampleMonotonic &&sample, CommitRealtime &&commit)
{
    if (auto status = check_realtime_setting(value, credentials); !status)
        return status;
    return time::commit_realtime_setting(time::EpochTime{value.seconds, static_cast<u32>(value.nanoseconds)},
                                         sample, commit);
}

// The kernel's form: `commit_now(epoch)` samples the monotonic clock and
// commits under the realtime lock (arch::set_realtime_now), so a reader on
// another CPU cannot advance the anchor past the sample in between.
template <typename CommitNow>
Status set_realtime_checked(ClockSetValue value, Credentials credentials, CommitNow &&commit_now)
{
    if (auto status = check_realtime_setting(value, credentials); !status)
        return status;
    return commit_now(time::EpochTime{value.seconds, static_cast<u32>(value.nanoseconds)});
}

} // namespace ok::user
