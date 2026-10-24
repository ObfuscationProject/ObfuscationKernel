#pragma once

#include "ok/time/epoch.hpp"

namespace ok::time
{

enum class RealtimeOrigin : u8 { none, boot_rtc, administrator_software };

struct RealtimeSample
{
    EpochTime value{};
    MonotonicPoint monotonic{};
    u64 wall_generation{};
    RealtimeOrigin origin{RealtimeOrigin::none};
};

struct RealtimeInfo
{
    AnchorState state{AnchorState::uninitialized};
    RealtimeOrigin origin{RealtimeOrigin::none};
    u64 wall_generation{};
    u64 monotonic_generation{};
    u64 conversion_resolution_ns{};
    Accuracy accuracy{Accuracy::unknown};
};

[[nodiscard]] inline Result<u64> next_wall_generation(u64 current)
{
    if (current == ~u64{0})
        return Status::overflow("wall-clock generation exhausted");
    return current + 1;
}

// One serialized boot-CPU owner. Neither an IRQ handler nor another CPU may
// enter during publication. Software selection never retries the physical RTC.
class RealtimeClock final
{
  public:
    RealtimeClock() = default;
    RealtimeClock(const RealtimeClock &) = delete;
    RealtimeClock &operator=(const RealtimeClock &) = delete;

    [[nodiscard]] Status boot_status() const { return boot_.status(); }
    [[nodiscard]] AnchorInfo boot_info() const { return boot_.info(); }
    [[nodiscard]] RealtimeInfo info() const { return info_; }

    Status reject_boot(Status reason)
    {
        if (boot_.info().state != AnchorState::uninitialized)
            return boot_.status();
        const auto result = boot_.reject(reason);
        info_.state = AnchorState::unavailable;
        failure_ = result;
        return result;
    }

    Status initialize_boot(EpochReading reading, MonotonicPoint before,
                           MonotonicPoint after, u64 conversion_resolution_ns)
    {
        if (boot_.info().state != AnchorState::uninitialized)
            return boot_.status();
        if (!conversion_resolution_ns)
            return reject_boot(Status::io_error("wall-clock conversion resolution unavailable"));
        const auto result = boot_.initialize(reading, before, after);
        if (!result)
        {
            info_.state = AnchorState::unavailable;
            failure_ = result;
            return result;
        }
        last_ = after;
        info_ = {AnchorState::ready, RealtimeOrigin::boot_rtc, 1, after.generation,
                 conversion_resolution_ns, Accuracy::unknown};
        return Status::success();
    }

    Status invalidate_boot(Status reason) { return boot_.invalidate(reason); }

    Status invalidate(Status reason)
    {
        if (info_.state == AnchorState::ready)
        {
            failure_ = reason ? Status::io_error("active realtime continuity invalidated") : reason;
            info_.state = AnchorState::invalidated;
            if (info_.origin == RealtimeOrigin::boot_rtc)
                static_cast<void>(boot_.invalidate(failure_));
        }
        return status();
    }

    // Trusted caller has already checked authority, Linux range and monotonic
    // floor. Validate mechanism invariants again before changing active state.
    Status set(EpochTime value, MonotonicPoint point, u64 conversion_resolution_ns)
    {
        if (boot_.info().state == AnchorState::uninitialized)
            return Status::not_initialized("boot epoch attempt must precede explicit setting");
        if (info_.state == AnchorState::invalidated)
            return failure_;
        if (value.nanoseconds >= nanoseconds_per_second || !point.generation || !conversion_resolution_ns)
            return Status::invalid_argument("invalid software epoch or monotonic anchor");
        if (info_.state == AnchorState::ready &&
            (point.generation != last_.generation || point.nanoseconds < last_.nanoseconds))
            return Status::io_error("setter monotonic continuity lost");
        auto generation = next_wall_generation(info_.wall_generation);
        if (!generation)
            return generation.status();
        software_epoch_ = value;
        software_anchor_ns_ = point.nanoseconds;
        last_ = point;
        info_ = {AnchorState::ready, RealtimeOrigin::administrator_software,
                 generation.value(), point.generation, conversion_resolution_ns, Accuracy::unknown};
        return Status::success();
    }

    [[nodiscard]] Result<RealtimeSample> read(MonotonicPoint point)
    {
        if (info_.state != AnchorState::ready)
            return status();
        if (point.generation != last_.generation || point.nanoseconds < last_.nanoseconds)
            return invalidate(Status::io_error("active realtime monotonic continuity lost"));
        auto value = info_.origin == RealtimeOrigin::boot_rtc
                         ? boot_.read(point)
                         : add_elapsed(software_epoch_, point.nanoseconds - software_anchor_ns_);
        if (!value)
            return invalidate(value.status());
        last_ = point;
        return RealtimeSample{value.value(), point, info_.wall_generation, info_.origin};
    }

    [[nodiscard]] Status status() const
    {
        if (info_.state == AnchorState::uninitialized)
            return Status::not_initialized("active realtime is not initialized");
        return info_.state == AnchorState::ready ? Status::success() : failure_;
    }

  private:
    EpochAnchor boot_{};
    RealtimeInfo info_{};
    EpochTime software_epoch_{};
    u64 software_anchor_ns_{};
    MonotonicPoint last_{};
    Status failure_{Status::not_initialized("active realtime is not initialized")};
};

} // namespace ok::time
