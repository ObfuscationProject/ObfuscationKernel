#pragma once

#include "ok/core/types.hpp"

namespace ok::time
{

// Converts `ticks` of a `frequency` Hz counter to nanoseconds, rounding down,
// without a 128-bit product: whole seconds and the fraction are scaled apart.
// Saturates instead of wrapping; a zero frequency gives zero.
[[nodiscard]] constexpr u64 ticks_to_nanoseconds(u64 ticks, u64 frequency)
{
    constexpr u64 second = 1000000000;
    constexpr u64 maximum = ~u64{0};
    if (!frequency)
        return 0;
    const u64 seconds = ticks / frequency;
    const u64 fraction = ticks % frequency;
    if (seconds > maximum / second)
        return maximum;
    // fraction * second fits while the frequency is below 2^64 / 10^9 Hz.
    const u64 part = frequency <= maximum / second ? fraction * second / frequency
                                                   : fraction / (frequency / second);
    return seconds * second > maximum - part ? maximum : seconds * second + part;
}

// A log-linear latency histogram in nanoseconds: values below 8 have a bucket
// each, larger ones eight buckets per power of two, so a percentile is exact
// to within one eighth of its value. Values from 2^32 ns (4.3 s) share the
// last bucket; the minimum, maximum and mean stay exact. Bucketing shifts only
// 32-bit values (a variable 64-bit shift is a libgcc call on MIPS o32).
// Recording is constant time and allocates nothing.
class LatencyHistogram
{
  public:
    static constexpr unsigned sub_buckets = 8;
    static constexpr unsigned maximum_exponent = 31;
    static constexpr usize bucket_count = sub_buckets + (maximum_exponent - 2) * sub_buckets;

    constexpr void record(u64 nanoseconds)
    {
        ++buckets_[index(nanoseconds)];
        if (!count_ || nanoseconds < minimum_)
            minimum_ = nanoseconds;
        if (nanoseconds > maximum_)
            maximum_ = nanoseconds;
        total_ = nanoseconds > ~u64{0} - total_ ? ~u64{0} : total_ + nanoseconds;
        ++count_;
    }
    [[nodiscard]] constexpr usize count() const { return count_; }
    [[nodiscard]] constexpr u64 minimum() const { return minimum_; }
    [[nodiscard]] constexpr u64 maximum() const { return maximum_; }
    [[nodiscard]] constexpr u64 mean() const { return count_ ? total_ / count_ : 0; }
    // The upper bound of the bucket holding the sample of rank
    // ceil(count * per_mille / 1000), clamped to the recorded range; zero
    // when empty.
    [[nodiscard]] constexpr u64 percentile(unsigned per_mille) const
    {
        if (!count_)
            return 0;
        const u64 rank = (static_cast<u64>(count_) * per_mille + 999) / 1000;
        u64 seen = 0;
        for (usize bucket = 0; bucket < bucket_count; ++bucket)
        {
            seen += buckets_[bucket];
            if (seen && seen >= rank)
            {
                const u64 bound = upper_bound(bucket);
                return bound < minimum_ ? minimum_ : bound > maximum_ ? maximum_ : bound;
            }
        }
        return maximum_;
    }
    [[nodiscard]] static constexpr usize index(u64 nanoseconds)
    {
        if (nanoseconds < sub_buckets)
            return static_cast<usize>(nanoseconds);
        if (nanoseconds > 0xffffffffu)
            return bucket_count - 1;
        const auto value = static_cast<u32>(nanoseconds);
        unsigned exponent = 31;
        while (!(value >> exponent))
            --exponent;
        const auto mantissa = static_cast<usize>((value >> (exponent - 3)) & (sub_buckets - 1));
        return sub_buckets + (exponent - 3) * sub_buckets + mantissa;
    }
    // The largest value `index()` maps to `bucket`.
    [[nodiscard]] static constexpr u64 upper_bound(usize bucket)
    {
        if (bucket < sub_buckets)
            return bucket;
        if (bucket >= bucket_count - 1)
            return ~u64{0};
        const auto exponent = static_cast<unsigned>((bucket - sub_buckets) / sub_buckets + 3);
        const auto mantissa = static_cast<u32>((bucket - sub_buckets) % sub_buckets);
        return static_cast<u64>(((sub_buckets + mantissa + 1) << (exponent - 3)) - 1);
    }

  private:
    u32 buckets_[bucket_count]{};
    usize count_{};
    u64 minimum_{};
    u64 maximum_{};
    u64 total_{};
};

} // namespace ok::time
