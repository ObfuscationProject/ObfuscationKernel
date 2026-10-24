#pragma once
#include "ok/arch/realtime.hpp"
#include "ok/arch/clock.hpp"

// Existing hosted runtime tests do not acquire a platform RTC. Their fake
// monotonic provider must never become an implicit UTC provider.
namespace ok::arch
{
Status initialize_realtime() { return Status::not_initialized("hosted RTC unavailable"); }
Result<time::EpochTime> realtime() { return Status::not_initialized("hosted RTC unavailable"); }
time::AnchorInfo realtime_info() { return {}; }
Result<time::MonotonicPoint> monotonic_sample()
{
    auto value = monotonic_nanoseconds();
    return value ? Result<time::MonotonicPoint>(time::MonotonicPoint{value.value(), 1})
                 : Result<time::MonotonicPoint>(value.status());
}
Status set_realtime(time::EpochTime, time::MonotonicPoint)
{
    return Status::unsupported("this hosted regression does not exercise clock setting");
}

} // namespace ok::arch
