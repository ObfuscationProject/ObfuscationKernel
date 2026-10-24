#pragma once

#include "ok/core/types.hpp"

namespace ok::time
{

inline constexpr u64 nanoseconds_per_second = 1000000000;

struct EpochTime
{
    i64 seconds{};
    u32 nanoseconds{};
    bool operator==(const EpochTime &) const = default;
};

struct Calendar
{
    u32 year{}, month{}, day{}, hour{}, minute{}, second{};
};

[[nodiscard]] constexpr bool leap_year(u32 year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

// UTC Gregorian fields, years 1..9999. Leap seconds require a separate policy.
[[nodiscard]] inline Result<EpochTime> calendar_epoch(Calendar value)
{
    constexpr u32 lengths[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (!value.year || value.year > 9999 || !value.month || value.month > 12 ||
        !value.day || value.hour > 23 || value.minute > 59 || value.second > 59)
        return Status::invalid_argument("invalid UTC calendar fields");
    const u32 last_day = lengths[value.month - 1] + (value.month == 2 && leap_year(value.year) ? 1 : 0);
    if (value.day > last_day)
        return Status::invalid_argument("invalid Gregorian calendar day");
    const i64 years = value.year - 1;
    i64 days = years * 365 + years / 4 - years / 100 + years / 400;
    for (u32 month = 1; month < value.month; ++month)
        days += lengths[month - 1] + (month == 2 && leap_year(value.year) ? 1 : 0);
    days += value.day - 1;
    days -= 719162; // Complete Gregorian days before 1970-01-01.
    return EpochTime{days * 86400 + value.hour * 3600 + value.minute * 60 + value.second, 0};
}

[[nodiscard]] inline Result<EpochTime> add_elapsed(EpochTime epoch, u64 elapsed_ns)
{
    if (epoch.nanoseconds >= nanoseconds_per_second)
        return Status::invalid_argument("unnormalized epoch nanoseconds");
    const u64 fraction = epoch.nanoseconds + elapsed_ns % nanoseconds_per_second;
    const i64 seconds = static_cast<i64>(elapsed_ns / nanoseconds_per_second + fraction / nanoseconds_per_second);
    constexpr i64 maximum = static_cast<i64>((~u64{0}) >> 1);
    if (epoch.seconds > maximum - seconds)
        return Status::overflow("UNIX epoch seconds exhausted");
    return EpochTime{epoch.seconds + seconds, static_cast<u32>(fraction % nanoseconds_per_second)};
}

struct MonotonicPoint
{
    u64 nanoseconds{};
    u64 generation{};
};

enum class EpochSource : u8 { unavailable, pl031, goldfish, ls7a };
enum class AnchorState : u8 { uninitialized, unavailable, ready, invalidated };
enum class Accuracy : u8 { unknown };

struct EpochReading
{
    EpochTime time{};
    EpochSource source{EpochSource::unavailable};
    u64 resolution_ns{};
};

struct AnchorInfo
{
    AnchorState state{AnchorState::uninitialized};
    EpochSource source{EpochSource::unavailable};
    u64 source_resolution_ns{};
    u64 acquisition_span_ns{};
    u64 monotonic_generation{};
    Accuracy accuracy{Accuracy::unknown};
};

// Serialized owner; copying an anchor would create an independent rollback check.
// A one-time anchor never retries, silently rebases, or repairs a lost generation.
class EpochAnchor final
{
  public:
    EpochAnchor() = default;
    EpochAnchor(const EpochAnchor &) = delete;
    EpochAnchor &operator=(const EpochAnchor &) = delete;

    static constexpr u64 maximum_acquisition_ns = 100000000;

    Status reject(Status reason)
    {
        if (info_.state != AnchorState::uninitialized)
            return status();
        failure_ = reason ? Status::invalid_argument("successful epoch rejection") : reason;
        info_.state = AnchorState::unavailable;
        return failure_;
    }

    Status initialize(EpochReading reading, MonotonicPoint before, MonotonicPoint after)
    {
        if (info_.state != AnchorState::uninitialized)
            return status();
        if (reading.time.nanoseconds >= nanoseconds_per_second ||
            reading.source == EpochSource::unavailable || reading.source > EpochSource::ls7a ||
            !reading.resolution_ns || reading.resolution_ns > nanoseconds_per_second ||
            !before.generation || before.generation != after.generation || after.nanoseconds < before.nanoseconds)
            return reject(Status::invalid_argument("invalid epoch source or monotonic bracket"));
        const u64 span = after.nanoseconds - before.nanoseconds;
        if (span > maximum_acquisition_ns)
            return reject(Status::io_error("epoch acquisition interval exceeded"));
        epoch_ = reading.time;
        anchor_ns_ = last_ns_ = after.nanoseconds;
        info_ = {AnchorState::ready, reading.source, reading.resolution_ns, span, after.generation, Accuracy::unknown};
        return Status::success();
    }

    Status invalidate(Status reason)
    {
        if (info_.state == AnchorState::ready)
        {
            failure_ = reason ? Status::io_error("epoch continuity invalidated") : reason;
            info_.state = AnchorState::invalidated;
        }
        return status();
    }

    [[nodiscard]] Result<EpochTime> read(MonotonicPoint point)
    {
        if (info_.state != AnchorState::ready)
            return status();
        if (point.generation != info_.monotonic_generation || point.nanoseconds < last_ns_)
            return invalidate(Status::io_error("epoch monotonic continuity lost"));
        auto result = add_elapsed(epoch_, point.nanoseconds - anchor_ns_);
        if (!result)
            return invalidate(result.status());
        last_ns_ = point.nanoseconds;
        return result;
    }

    [[nodiscard]] Status status() const
    {
        if (info_.state == AnchorState::uninitialized)
            return Status::not_initialized("epoch anchor is not initialized");
        return info_.state == AnchorState::ready ? Status::success() : failure_;
    }
    [[nodiscard]] AnchorInfo info() const { return info_; }

  private:
    AnchorInfo info_{};
    EpochTime epoch_{};
    u64 anchor_ns_{}, last_ns_{};
    Status failure_{Status::not_initialized("epoch anchor is not initialized")};
};

} // namespace ok::time
