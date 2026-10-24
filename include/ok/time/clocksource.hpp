#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/time/epoch.hpp"

#include <concepts>

namespace ok::time
{

// A free-running counter with a known frequency. Each architecture supplies
// one (TSC/HPET, CNTVCT, rdtime, stable counter, CP0 Count, timebase) as a
// static backend; the calibrated clock is instantiated over it. Monotonic time
// derives from the counter alone, so it advances while the CPU idles; the old
// 10 ms tick count did not.
template <typename Source>
concept ClockSource = requires(const Source source) {
    { source.counter() } -> std::same_as<u64>;
    { source.frequency_hz() } -> std::same_as<u64>;
    { source.name() } -> std::convertible_to<std::string_view>;
};

class ModelClockSource final
{
  public:
    explicit ModelClockSource(u64 frequency_hz) : frequency_(frequency_hz)
    {
    }
    [[nodiscard]] u64 counter() const
    {
        return counter_;
    }
    [[nodiscard]] u64 frequency_hz() const
    {
        return frequency_;
    }
    [[nodiscard]] std::string_view name() const
    {
        return "model-clock-source";
    }
    void advance(u64 ticks)
    {
        counter_ += ticks;
    }

  private:
    u64 frequency_{};
    u64 counter_{};
};
static_assert(ClockSource<ModelClockSource>);

// Converts counter ticks to nanoseconds without 128-bit arithmetic so the same
// code serves the 32-bit profiles: whole seconds first, then the remainder.
template <ClockSource Source> class CalibratedClock final : private NonCopyable
{
  public:
    explicit CalibratedClock(const Source &source) : source_(source)
    {
    }

    Status initialize()
    {
        if (source_.frequency_hz() == 0)
            return Status{StatusCode::invalid_argument, "clock source frequency unknown"};
        origin_ = source_.counter();
        initialized_ = true;
        return Status::success();
    }

    [[nodiscard]] Result<u64> monotonic_ns() const
    {
        if (!initialized_)
            return Status{StatusCode::not_initialized, "calibrated clock not initialised"};
        const u64 ticks = source_.counter() - origin_;
        const u64 hz = source_.frequency_hz();
        const u64 seconds = ticks / hz;
        const u64 remainder = ticks % hz;
        if (seconds > (~u64{0}) / nanoseconds_per_second)
            return Status{StatusCode::overflow, "monotonic nanoseconds exhausted"};
        const u64 whole = seconds * nanoseconds_per_second;
        // remainder < hz; remainder * 1e9 fits in u64 for hz < ~1.8e10.
        if (hz > (~u64{0}) / nanoseconds_per_second)
            return Status{StatusCode::unsupported, "clock frequency above 18 GHz needs wide arithmetic"};
        return whole + (remainder * nanoseconds_per_second) / hz;
    }

    [[nodiscard]] Result<u64> deadline_ticks(u64 relative_ns) const
    {
        if (!initialized_)
            return Status{StatusCode::not_initialized, "calibrated clock not initialised"};
        const u64 hz = source_.frequency_hz();
        const u64 seconds = relative_ns / nanoseconds_per_second;
        const u64 rem = relative_ns % nanoseconds_per_second;
        if (hz != 0 && seconds > (~u64{0}) / hz)
            return Status{StatusCode::overflow, "deadline exceeds counter range"};
        const u64 ticks = seconds * hz + (rem * hz) / nanoseconds_per_second;
        return source_.counter() + ticks;
    }

    [[nodiscard]] bool initialized() const
    {
        return initialized_;
    }

  private:
    const Source &source_;
    u64 origin_{};
    bool initialized_{false};
};

} // namespace ok::time
