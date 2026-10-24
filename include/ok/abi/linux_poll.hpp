#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/abi/linux_sleep.hpp"
#include "ok/user/native_poll.hpp"

namespace ok::abi::linux_user
{
template <Profile Selected>
    requires SupportedProfile<Selected>
struct PollCodec
{
    using C = Codec<Selected>;
    static constexpr bool mips = Selected == Profile::mips || Selected == Profile::mips64;
    static constexpr usize record_size = 8;
    static constexpr usize signal_bytes = mips ? 16 : 8;
    static constexpr u16 read_normal = 0x40;
    static constexpr u16 write_normal = mips ? 4 : 0x100;

    static u64 word(std::span<const u8> bytes)
    {
        u64 value{};
        for (usize i = 0; i < bytes.size(); ++i)
            value |= u64{bytes[i]} << (8 * (C::profile.big_endian ? bytes.size() - 1 - i : i));
        return value;
    }
    static ok_pollfd decode(std::span<const u8, record_size> bytes)
    {
        return {std::bit_cast<i32>(static_cast<u32>(word(bytes.first(4)))),
                std::bit_cast<i16>(static_cast<u16>(word(bytes.subspan(4, 2)))), 0};
    }
    static posix::ClockTime end_time(u64 now, Timespec interval)
    {
        constexpr u64 second = 1000000000;
        constexpr u64 maximum = 0x7fffffffffffffffULL;
        const u64 nanos = now % second + static_cast<u64>(interval.nanoseconds);
        const u64 elapsed = now / second + nanos / second;
        if (static_cast<u64>(interval.seconds) > maximum - elapsed)
            return {static_cast<i64>(maximum), 0};
        return {interval.seconds + static_cast<i64>(elapsed), static_cast<i64>(nanos % second)};
    }
};
} // namespace ok::abi::linux_user
