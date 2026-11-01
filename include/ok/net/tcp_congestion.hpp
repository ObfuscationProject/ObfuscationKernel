#pragma once

#include <ok/core/types.hpp>
#include <concepts>

namespace ok::net::tcp
{
// Congestion control is a static policy of the connection. The connection
// detects the events (acknowledged bytes, the third duplicate ACK, partial and
// full acknowledgments during recovery, retransmission timeout) and owns the
// recovery point; the policy owns the windows.
template <class Policy>
concept CongestionControl = std::default_initializable<Policy> && requires(Policy &policy, const Policy &view, u32 bytes, u32 flight, u32 mss)
{
    { policy.start(mss) } -> std::same_as<void>;
    { policy.acknowledged(bytes, mss) } -> std::same_as<void>;
    { policy.enter_recovery(flight, mss) } -> std::same_as<void>;
    { policy.duplicate(mss) } -> std::same_as<void>;
    { policy.partial(bytes, mss) } -> std::same_as<void>;
    { policy.exit_recovery(flight, mss) } -> std::same_as<void>;
    { policy.timeout(flight, mss) } -> std::same_as<void>;
    { view.window() } -> std::same_as<u32>;
    { view.threshold() } -> std::same_as<u32>;
};

// RFC 5681 slow start and congestion avoidance with the RFC 6582 NewReno
// fast recovery window rules.
class NewReno final
{
  public:
    void start(u32 mss)
    {
        // RFC 5681 section 3.1 initial window.
        const u32 limit = 4380 > 2 * mss ? 4380 : 2 * mss;
        cwnd_ = 4 * mss < limit ? 4 * mss : limit; ssthresh_ = ~u32{0};
    }
    void acknowledged(u32 bytes, u32 mss)
    {
        if (cwnd_ < ssthresh_) grow(bytes < mss ? bytes : mss);
        else { const u32 step = static_cast<u32>(u64{mss} * mss / cwnd_); grow(step ? step : 1); }
    }
    void enter_recovery(u32 flight, u32 mss) { ssthresh_ = half(flight, mss); cwnd_ = ssthresh_; grow(3 * mss); }
    void duplicate(u32 mss) { grow(mss); }
    // Deflate by the newly acknowledged data, then add back one segment.
    void partial(u32 bytes, u32 mss) { cwnd_ = cwnd_ > bytes ? cwnd_ - bytes : 0; grow(mss); }
    void exit_recovery(u32 flight, u32 mss)
    { const u32 pipe = (flight > mss ? flight : mss) + mss; cwnd_ = ssthresh_ < pipe ? ssthresh_ : pipe; }
    void timeout(u32 flight, u32 mss) { ssthresh_ = half(flight, mss); cwnd_ = mss; }
    [[nodiscard]] u32 window() const { return cwnd_; }
    [[nodiscard]] u32 threshold() const { return ssthresh_; }

  private:
    static u32 half(u32 flight, u32 mss) { return flight / 2 > 2 * mss ? flight / 2 : 2 * mss; }
    void grow(u32 bytes) { cwnd_ = cwnd_ > ~u32{0} - bytes ? ~u32{0} : cwnd_ + bytes; }
    u32 cwnd_{}, ssthresh_{~u32{0}};
};
static_assert(CongestionControl<NewReno>);
}
