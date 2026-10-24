#pragma once

#include "ok/core/types.hpp"

namespace ok::sched
{
// A boot-CPU wait owns one absolute deadline. Generation tokens make a canceled
// or previously completed registration unusable after the record is rearmed.
class DeadlineWait
{
  public:
    Result<u64> arm(u64 now, i64 seconds, i64 nanoseconds, bool absolute)
    {
        constexpr u64 second = 1000000000;
        if (seconds < 0 || nanoseconds < 0 || nanoseconds >= static_cast<i64>(second))
            return Status::invalid_argument("invalid sleep interval");
        const auto sec = static_cast<u64>(seconds);
        const auto ns = static_cast<u64>(nanoseconds);
        if (sec > (~u64{0} - ns) / second)
            return Status::overflow("sleep interval overflows clock");
        const auto interval = sec * second + ns;
        if (!absolute && interval > ~u64{0} - now)
            return Status::overflow("sleep deadline overflows clock");
        if (armed_)
            return Status::busy("thread already owns a deadline");
        if (generation_ == ~u64{0})
            return Status::overflow("wait generation exhausted");
        deadline_ = absolute ? interval : now + interval;
        armed_ = true;
        return ++generation_;
    }
    bool complete(u64 now, u64 generation)
    {
        if (!armed_ || generation != generation_ || now < deadline_)
            return false;
        armed_ = false;
        return true;
    }
    void cancel()
    {
        armed_ = false;
    }
    [[nodiscard]] bool armed() const { return armed_; }
    [[nodiscard]] u64 generation() const { return generation_; }
    [[nodiscard]] u64 deadline() const { return deadline_; }

  private:
    u64 generation_{};
    u64 deadline_{};
    bool armed_{};
};
} // namespace ok::sched
