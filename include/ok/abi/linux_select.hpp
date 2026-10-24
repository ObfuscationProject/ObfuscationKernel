#pragma once

#include "ok/abi/linux_poll.hpp"
#include "ok/sched/resources.hpp"

namespace ok::abi::linux_user
{
template <Profile Selected>
    requires SupportedProfile<Selected>
struct SelectCodec
{
    using C = Codec<Selected>;
    using Poll = PollCodec<Selected>;
    static_assert(sched::max_process_fds == 32, "Select bitmap capacity must follow descriptor table growth");
    static constexpr usize word_bytes = C::profile.word_bytes;
    static constexpr bool old_select = Selected == Profile::i386 || Selected == Profile::ppc;
    static constexpr usize old_argument_bytes = 5 * word_bytes;
    static constexpr usize signal_argument_bytes = 2 * word_bytes;
    static constexpr usize mask_bytes(usize count) { return count ? word_bytes : 0; }
    static constexpr u32 valid_bits(usize count)
    {
        return count == 32 ? ~u32{0} : count ? (u32{1} << count) - 1 : 0;
    }
    static i64 signed_word(u64 value)
    {
        if constexpr (word_bytes == 4) return std::bit_cast<i32>(static_cast<u32>(value));
        return std::bit_cast<i64>(value);
    }
    static std::array<u64, 5> old_arguments(std::span<const u8, old_argument_bytes> bytes)
    {
        std::array<u64, 5> result{};
        for (usize i = 0; i < result.size(); ++i)
            result[i] = Poll::word(bytes.subspan(i * word_bytes, word_bytes));
        return result;
    }
    static Result<Timespec> timeval(std::span<const u8> bytes)
    {
        if (bytes.size() != 2 * word_bytes) return Status::invalid_argument("Linux timeval size");
        const auto seconds = signed_word(Poll::word(bytes.first(word_bytes)));
        const auto micros = signed_word(Poll::word(bytes.subspan(word_bytes)));
        // Native Linux long arithmetic wraps before conversion to time64_t.
        // Unsigned addition preserves that wire behavior without C++ signed UB.
        const auto normalized = signed_word(static_cast<u64>(seconds) + static_cast<u64>(micros / 1000000));
        const auto nanos = (micros % 1000000) * 1000;
        if (normalized < 0 || nanos < 0) return Status::invalid_argument("Linux timeval interval");
        return Timespec{normalized, nanos};
    }
};
} // namespace ok::abi::linux_user
