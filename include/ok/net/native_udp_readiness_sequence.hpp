#pragma once

#include "ok/core/types.hpp"

namespace ok::net
{

struct NativeUdpReadinessCapacity
{
    u64 maximum{};
};

// An ordinary zero-born bounded counter. Small-limit controls exercise the
// same saturation operation without setting a private production history.
template <u64 DefaultMaximum = ~u64{0}>
class NativeUdpReadinessSequence final
{
  public:
    static_assert(DefaultMaximum > 0);
    NativeUdpReadinessSequence() = default;
    // Capacity belongs to a new zero-born owner and can never change later.
    explicit NativeUdpReadinessSequence(NativeUdpReadinessCapacity capacity)
        : maximum_{capacity.maximum}, terminal_{capacity.maximum == 0} {}
    NativeUdpReadinessSequence(const NativeUdpReadinessSequence &) = delete;
    NativeUdpReadinessSequence &operator=(const NativeUdpReadinessSequence &) = delete;
    NativeUdpReadinessSequence(NativeUdpReadinessSequence &&) = delete;
    NativeUdpReadinessSequence &operator=(NativeUdpReadinessSequence &&) = delete;

    [[nodiscard]] u64 value() const { return value_; }
    [[nodiscard]] bool terminal() const { return terminal_; }
    [[nodiscard]] bool unchanged(u64 observed) const
    {
        return !terminal_ && observed == value_;
    }
    bool advance()
    {
        if (terminal_) return false;
        if (value_ == maximum_)
        {
            terminal_ = true;
            return false;
        }
        ++value_;
        return true;
    }

  private:
    const u64 maximum_{DefaultMaximum};
    u64 value_{};
    bool terminal_{};
};

} // namespace ok::net
