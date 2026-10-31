#pragma once

#include "ok/abi/linux_user.hpp"

namespace ok::abi::linux_user
{

// Nanosecond pair of a Linux itimerval or itimerspec record (interval first on
// the wire, as in both kernel structures).
struct TimerValue
{
    u64 interval{};
    u64 value{};
    constexpr bool operator==(const TimerValue &) const = default;
};

// The leading part of struct sigevent the kernel reads: the word-sized value
// (kept as target-order bits), signal, notification and SIGEV_THREAD_ID tid.
struct SignalEvent
{
    u64 value{};
    i32 signal{};
    i32 notify{};
    i32 thread{};
};

inline constexpr i32 sigev_signal = 0, sigev_none = 1, sigev_thread = 2, sigev_thread_id = 4;

template <Profile Selected>
    requires SupportedProfile<Selected>
struct TimerCodec
{
    using Abi = Codec<Selected>;
    static constexpr usize word = Abi::profile.word_bytes;
    // __kernel_old_itimerval: four target longs.
    static constexpr usize itimerval_bytes = 4 * word;
    // timer_create copies the whole SIGEV_MAX_SIZE record.
    static constexpr usize sigevent_bytes = 64;
    [[nodiscard]] static constexpr usize itimerspec_bytes(TimeFormat format)
    {
        return 2 * Abi::timespec_size(format);
    }

    // get_itimerval: whole seconds and microseconds below one second.
    [[nodiscard]] static Result<TimerValue> decode_itimerval(std::span<const u8> bytes)
    {
        if (bytes.size() != itimerval_bytes) return Status::invalid_argument("Linux itimerval size");
        auto field = [&](usize index) { return signed_word(read(bytes.subspan(index * word, word))); };
        const i64 interval_seconds = field(0), interval_micro = field(1);
        const i64 value_seconds = field(2), value_micro = field(3);
        if (interval_seconds < 0 || value_seconds < 0 || interval_micro < 0 || value_micro < 0 ||
            interval_micro >= 1000000 || value_micro >= 1000000)
            return Status::invalid_argument("Linux itimerval timeval");
        return TimerValue{nanoseconds(interval_seconds, interval_micro * 1000),
                          nanoseconds(value_seconds, value_micro * 1000)};
    }
    // put_itimerval truncates nanoseconds to microseconds and seconds to a long.
    static void encode_itimerval(std::span<u8, itimerval_bytes> bytes, TimerValue setting)
    {
        const u64 words[4]{setting.interval / second, setting.interval % second / 1000,
                           setting.value / second, setting.value % second / 1000};
        for (usize index = 0; index < 4; ++index) write(bytes.subspan(index * word, word), words[index]);
    }

    // get_itimerspec64 then the timespec64_valid check of do_timer_settime.
    [[nodiscard]] static Result<TimerValue> decode_itimerspec(std::span<const u8> bytes, TimeFormat format)
    {
        const usize half = Abi::timespec_size(format);
        if (bytes.size() != 2 * half) return Status::invalid_argument("Linux itimerspec size");
        auto interval = Abi::decode_timespec(bytes.first(half), format);
        auto value = Abi::decode_timespec(bytes.subspan(half), format);
        if (!interval || !value || interval.value().seconds < 0 || value.value().seconds < 0)
            return Status::invalid_argument("Linux itimerspec timespec");
        return TimerValue{nanoseconds(interval.value().seconds, interval.value().nanoseconds),
                          nanoseconds(value.value().seconds, value.value().nanoseconds)};
    }
    // put_itimerspec64; a legacy record keeps the low 32 bits of the seconds.
    static Status encode_itimerspec(std::span<u8> bytes, TimeFormat format, TimerValue setting)
    {
        const usize half = Abi::timespec_size(format);
        if (bytes.size() != 2 * half) return Status::invalid_argument("Linux itimerspec size");
        auto timespec = [&](u64 value) {
            i64 seconds = static_cast<i64>(value / second);
            if (format == TimeFormat::legacy_timespec && word == 4)
                seconds = static_cast<i32>(static_cast<u32>(seconds));
            return Timespec{seconds, static_cast<i64>(value % second)};
        };
        if (auto status = Abi::encode_timespec(bytes.first(half), format, timespec(setting.interval)); !status)
            return status;
        return Abi::encode_timespec(bytes.subspan(half), format, timespec(setting.value));
    }

    [[nodiscard]] static constexpr SignalEvent decode_sigevent(std::span<const u8, sigevent_bytes> bytes)
    {
        auto integer = [&](usize offset) { return static_cast<i32>(static_cast<u32>(read(bytes.subspan(offset, 4)))); };
        return {read(bytes.first(word)), integer(word), integer(word + 4), integer(word + 8)};
    }

  private:
    static constexpr u64 second = 1000000000;
    // timespec64_to_ktime saturates at KTIME_MAX.
    [[nodiscard]] static constexpr u64 nanoseconds(i64 seconds, i64 nanos)
    {
        constexpr u64 horizon = 0x7fffffffffffffffULL;
        const auto whole = static_cast<u64>(seconds);
        return whole >= horizon / second ? horizon : whole * second + static_cast<u64>(nanos);
    }
    [[nodiscard]] static constexpr i64 signed_word(u64 value)
    {
        return word == 4 ? static_cast<i64>(static_cast<i32>(static_cast<u32>(value))) : static_cast<i64>(value);
    }
    [[nodiscard]] static constexpr u64 read(std::span<const u8> bytes)
    {
        u64 result = 0;
        for (usize index = 0; index < bytes.size(); ++index)
            result |= static_cast<u64>(bytes[index])
                << (8 * (Abi::profile.big_endian ? bytes.size() - 1 - index : index));
        return result;
    }
    static constexpr void write(std::span<u8> bytes, u64 value)
    {
        for (usize index = 0; index < bytes.size(); ++index)
            bytes[index] = static_cast<u8>(value >> (8 * (Abi::profile.big_endian ? bytes.size() - 1 - index : index)));
    }
};

} // namespace ok::abi::linux_user
