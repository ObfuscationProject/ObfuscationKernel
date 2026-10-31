#pragma once

#include "ok/core/types.hpp"

namespace ok::time
{

// Timer arithmetic runs in nanoseconds of one clock base and saturates at the
// Linux KTIME_MAX horizon.
inline constexpr u64 timer_horizon = 0x7fffffffffffffffULL;
inline constexpr i32 timer_overrun_limit = 0x7fffffff;

struct TimerSetting
{
    u64 value{}; // Time to the next expiry; zero is disarmed.
    u64 interval{};
    constexpr bool operator==(const TimerSetting &) const = default;
};

// One interval timer with the Linux 6.13 signal contract. An expiry queues one
// signal; an interval timer rearms only when that signal is dequeued, and the
// intervals it missed meanwhile become the overrun the signal reports
// (posixtimer_deliver_signal). A quiet timer (SIGEV_NONE) never expires and is
// only observed. The owner reads the clock, queues and refreshes the signal and
// reports its dequeue; this class holds no signal state.
class IntervalTimer
{
  public:
    enum class State : u8
    {
        disarmed,
        armed,
        signalled, // An interval timer whose signal has not been dequeued yet.
    };

    constexpr void reset(bool quiet)
    {
        *this = {};
        quiet_ = quiet;
    }
    // timer_settime/setitimer after the caller read the old setting: a zero
    // value disarms and clears the interval; overrun accounting restarts.
    constexpr void set(u64 now, TimerSetting next, bool absolute)
    {
        overrun_ = -1;
        overrun_last_ = 0;
        if (!next.value)
        {
            state_ = State::disarmed;
            interval_ = 0;
            return;
        }
        interval_ = next.interval > timer_horizon ? timer_horizon : next.interval;
        expiry_ = absolute ? (next.value > timer_horizon ? timer_horizon : next.value) : saturate(now, next.value);
        state_ = State::armed;
    }
    // timer_gettime (common_timer_get). An expired signalled or quiet interval
    // timer moves forward past now, counting the skipped intervals as overrun.
    constexpr TimerSetting get(u64 now)
    {
        if (state_ == State::disarmed) return {};
        TimerSetting result{0, interval_};
        if (interval_ && (state_ == State::signalled || quiet_)) count(forward(now));
        if (expiry_ > now) result.value = expiry_ - now;
        else if (!quiet_) result.value = 1; // The signal has not been delivered yet.
        return result;
    }
    // getitimer (itimer_get_remtime): an expired timer is inactive until its
    // signal is dequeued; an active one past its expiry reports a microsecond.
    [[nodiscard]] constexpr TimerSetting itimer(u64 now) const
    {
        if (state_ != State::armed) return {0, interval_};
        return {expiry_ > now ? expiry_ - now : 1000, interval_};
    }
    // The owner queues one signal when this returns true.
    constexpr bool expire(u64 now)
    {
        if (quiet_ || state_ != State::armed || now < expiry_) return false;
        state_ = interval_ ? State::signalled : State::disarmed;
        previewed_ = now;
        return true;
    }
    // The si_overrun the queued signal reports if it is dequeued at now. The
    // owner refreshes its queued record with this until the record is gone.
    constexpr i32 preview(u64 now)
    {
        previewed_ = now;
        if (state_ != State::signalled) return 0;
        const u64 skipped = now < expiry_ ? 0 : (now - expiry_) / interval_ + 1;
        return clamp(add(overrun_, skipped));
    }
    // The queued signal was dequeued at the last preview: rearm past it.
    constexpr void dequeued()
    {
        if (state_ != State::signalled) return;
        count(forward(previewed_));
        overrun_last_ = overrun_;
        overrun_ = -1;
        state_ = State::armed;
    }
    // An ignored ITIMER_REAL signal is dropped, so nothing dequeues it: the
    // timer stays inactive with its interval, like an hrtimer never restarted.
    constexpr void abandon()
    {
        state_ = State::disarmed;
    }
    [[nodiscard]] constexpr i32 overrun() const { return clamp(overrun_last_); }
    [[nodiscard]] constexpr State state() const { return state_; }
    [[nodiscard]] constexpr bool quiet() const { return quiet_; }
    [[nodiscard]] constexpr u64 expiry() const { return expiry_; }
    [[nodiscard]] constexpr u64 interval() const { return interval_; }

  private:
    [[nodiscard]] static constexpr u64 saturate(u64 base, u64 delta)
    {
        return base >= timer_horizon || delta > timer_horizon - base ? timer_horizon : base + delta;
    }
    [[nodiscard]] static constexpr i64 add(i64 overrun, u64 skipped)
    {
        constexpr i64 maximum = 0x7fffffffffffffffLL;
        // An overrun is at least -1, so its headroom fits in unsigned arithmetic.
        const u64 headroom = static_cast<u64>(maximum) - static_cast<u64>(overrun);
        return skipped > headroom ? maximum : static_cast<i64>(static_cast<u64>(overrun) + skipped);
    }
    [[nodiscard]] static constexpr i32 clamp(i64 overrun)
    {
        return overrun > timer_overrun_limit ? timer_overrun_limit : overrun < 0 ? 0 : static_cast<i32>(overrun);
    }
    // hrtimer_forward: the least k with expiry + k * interval > now.
    constexpr u64 forward(u64 now)
    {
        if (!interval_ || now < expiry_) return 0;
        const u64 skipped = (now - expiry_) / interval_ + 1;
        const u64 delta = skipped > timer_horizon / interval_ ? timer_horizon : skipped * interval_;
        expiry_ = saturate(expiry_, delta);
        return skipped;
    }
    constexpr void count(u64 skipped)
    {
        overrun_ = add(overrun_, skipped);
    }

    u64 expiry_{};
    u64 interval_{};
    u64 previewed_{};
    i64 overrun_{-1};
    i64 overrun_last_{};
    State state_{State::disarmed};
    bool quiet_{};
};

} // namespace ok::time
