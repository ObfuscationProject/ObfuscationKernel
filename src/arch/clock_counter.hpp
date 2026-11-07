#pragma once

#include "ok/core/types.hpp"

#include <atomic>

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
        // A 64-bit step past half its range is no wrap but a CPU whose own
        // counter (the x86 TSC) runs behind the last reader's: time stands
        // still until that counter catches up.
        if (mask_ == max_value && delta > max_value / 2)
            return elapsed_;
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

// A 64-bit counter read on several CPUs, each from its own copy (the x86 TSC,
// corrected per CPU at bring-up) or from one shared counter. Conversion keeps
// no state, so readers need no lock; the highest reading returned so far is a
// floor no later reading on any CPU falls below, so a CPU whose counter trails
// another's stands still until it catches up. Needs lock-free 64-bit atomics.
class SharedCounterClock final
{
  public:
    Status initialize(u64 frequency, u64 counter, unsigned width = 64)
    {
        if (frequency_ != 0)
            return Status::success();
        // The remainder's scaling below needs frequency * 10^9 to fit.
        if (frequency == 0 || frequency > max_value / (nanoseconds_per_second + 1) || width != 64)
            return Status::invalid_argument("invalid platform clock frequency or width");
        base_ = counter;
        frequency_ = frequency;
        return Status::success();
    }

    Result<u64> sample(u64 counter)
    {
        if (!initialized())
            return Status::not_initialized("platform clock is not initialized");
        auto floor = floor_.load(std::memory_order_acquire);
        const auto delta = counter - base_;
        // Past half the range is a counter behind the epoch, not a wrap.
        if (delta > max_value / 2)
            return floor;
        const auto seconds = delta / frequency_;
        const auto partial = delta % frequency_ * nanoseconds_per_second / frequency_;
        if (seconds > (max_value - partial) / nanoseconds_per_second)
            return Status::overflow("platform monotonic clock exhausted");
        const auto value = seconds * nanoseconds_per_second + partial;
        while (floor < value &&
               !floor_.compare_exchange_weak(floor, value, std::memory_order_acq_rel, std::memory_order_acquire))
        {
        }
        return floor < value ? value : floor;
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
    // Written once on the boot CPU before any other CPU reads the clock.
    u64 frequency_{};
    u64 base_{};
    std::atomic<u64> floor_{};
};

} // namespace ok::arch::detail
