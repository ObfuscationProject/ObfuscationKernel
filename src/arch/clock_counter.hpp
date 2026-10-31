#pragma once

#include "ok/core/types.hpp"

namespace ok::arch::detail
{

// Shared integer conversion, independently testable without privileged reads.
// A narrow counter must be sampled at least once per hardware wrap. Retaining
// the fractional numerator prevents frequent reads from discarding elapsed time.
class CounterClock final
{
  public:
    Status initialize(u64 frequency, u64 counter, unsigned width = 64)
    {
        if (frequency_ != 0)
            return Status::success();
        if (frequency == 0 || frequency > max_value / (nanoseconds_per_second + 1) ||
            (width != 32 && width != 64))
            return Status::invalid_argument("invalid platform clock frequency or width");
        mask_ = width == 32 ? 0xffffffffu : max_value;
        previous_ = counter & mask_;
        frequency_ = frequency;
        return Status::success();
    }

    Result<u64> sample(u64 counter)
    {
        if (!initialized())
            return Status::not_initialized("platform clock is not initialized");
        const auto current = counter & mask_;
        const auto delta = (current - previous_) & mask_;
        const auto seconds = delta / frequency_;
        const auto fraction = (delta % frequency_) * nanoseconds_per_second + remainder_;
        const auto partial = fraction / frequency_;
        if (seconds > (max_value - elapsed_) / nanoseconds_per_second)
            return Status::overflow("platform monotonic clock exhausted");
        const auto whole = seconds * nanoseconds_per_second;
        if (partial > max_value - elapsed_ - whole)
            return Status::overflow("platform monotonic clock exhausted");
        elapsed_ += whole + partial;
        remainder_ = fraction % frequency_;
        previous_ = current;
        return elapsed_;
    }

    [[nodiscard]] bool initialized() const { return frequency_ != 0; }
    [[nodiscard]] u64 frequency() const { return frequency_; }
    [[nodiscard]] u64 resolution() const
    {
        return initialized() ? (nanoseconds_per_second + frequency_ - 1) / frequency_ : 0;
    }

  private:
    static constexpr u64 max_value = ~u64{0};
    static constexpr u64 nanoseconds_per_second = 1000000000;
    u64 frequency_{};
    u64 previous_{};
    u64 elapsed_{};
    u64 remainder_{};
    u64 mask_{};
};

} // namespace ok::arch::detail
