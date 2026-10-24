#pragma once
#include "ok/time/epoch.hpp"
#include "ok/time/public_clock.hpp"

namespace ok::tests
{
inline Status check_realtime_model()
{
    auto date = time::calendar_epoch({2024, 2, 29, 12, 34, 56});
    if (!date || date.value().seconds != 1709210096 || time::calendar_epoch({2100, 2, 29, 0, 0, 0}))
        return Status::fault("Gregorian epoch model failed");
    time::EpochAnchor anchor;
    if (!anchor.initialize({date.value(), time::EpochSource::pl031, 1000000000}, {0, 1}, {5, 1}))
        return Status::fault("epoch anchor model failed");
    auto later = anchor.read({1000000005, 1});
    if (!later || later.value().seconds != 1709210097 || anchor.read({0, 2}) ||
        anchor.info().state != time::AnchorState::invalidated || anchor.read({2000000005, 1}))
        return Status::fault("epoch continuity model failed");
    if (time::native_time_seconds<4>({2147483648LL, 0}) || time::native_time_seconds<8>({-1, 0}) ||
        time::native_time_seconds<4>({2147483647LL, 0}).value() != 2147483647LL ||
        time::native_time_seconds<8>({2214176523LL, 0}).value() != 2214176523LL)
        return Status::fault("native legacy time return model failed");
    return Status::success();
}
} // namespace ok::tests
