#pragma once

#include "ok/arch/clock.hpp"
#include "ok/arch/realtime.hpp"

namespace ok::time
{

// Hardware clock provider. Trusted scheduler models retain their own monotonic
// ticks; neither cached ticks nor RTC precision imply an accurate UTC clock.
[[nodiscard]] inline Result<EpochTime> public_clock(u32 identity, bool resolution = false)
{
    if (identity > 1)
        return Status::invalid_argument("unsupported clock identity");
    EpochTime value{};
    if (identity == 0)
    {
        auto current = arch::realtime();
        if (!current)
            return Status::io_error("realtime epoch is unavailable");
        value = current.value();
    }
    else
    {
        auto current = arch::monotonic_nanoseconds();
        if (!current)
            return Status::io_error("monotonic epoch is unavailable");
        value = {static_cast<i64>(current.value() / nanoseconds_per_second),
                 static_cast<u32>(current.value() % nanoseconds_per_second)};
    }
    if (resolution)
    {
        const u64 step = arch::clock_resolution_ns();
        if (!step)
            return Status::io_error("clock resolution is unavailable");
        value = {static_cast<i64>(step / nanoseconds_per_second),
                 static_cast<u32>(step % nanoseconds_per_second)};
    }
    return value;
}

// The legacy native time syscall returns one signed target word. Avoid both
// narrowing and successful timestamps that collide with its errno encoding.
template <usize WordBytes>
    requires (WordBytes == 4 || WordBytes == 8)
[[nodiscard]] inline Result<i64> native_time_seconds(EpochTime value)
{
    if constexpr (WordBytes == 4)
        if (value.seconds < -2147483648LL || value.seconds > 2147483647LL)
            return Status::overflow("legacy native time return width");
    if (value.seconds < 0 && value.seconds >= -4095)
        return Status::overflow("legacy native time errno collision");
    return value.seconds;
}

} // namespace ok::time
