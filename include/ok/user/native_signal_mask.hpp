#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/sched/signal_mask.hpp"
#include <array>
#include <span>

namespace ok::user::native
{

using SignalMask = sched::SignalMask;

template <abi::linux_user::Profile Profile>
struct SignalMaskCodec
{
    using Abi = abi::linux_user::Codec<Profile>;
    static constexpr bool mips = Profile == abi::linux_user::Profile::mips || Profile == abi::linux_user::Profile::mips64;
    static constexpr usize bytes = mips ? 16 : 8;
    static constexpr usize word_bytes = Abi::profile.word_bytes;
    static constexpr u32 stop = mips ? 23 : 19;
    static constexpr i32 block = mips ? 1 : 0, unblock = block + 1, replace = block + 2;

    static constexpr SignalMask sanitize(SignalMask mask)
    {
        if constexpr (!mips) mask.words[1] = 0;
        mask.set(9, false); mask.set(stop, false);
        return mask;
    }
    static constexpr SignalMask decode(const std::array<std::byte, bytes> &input)
    {
        SignalMask mask;
        for (usize word = 0; word < bytes / word_bytes; ++word)
        {
            u64 value{};
            for (usize offset = 0; offset < word_bytes; ++offset)
                value |= static_cast<u64>(input[word * word_bytes + offset]) <<
                    (8 * (Abi::profile.big_endian ? word_bytes - 1 - offset : offset));
            for (usize bit = 0; bit < word_bytes * 8; ++bit)
                if (value & (u64{1} << bit)) mask.set(static_cast<u32>(word * word_bytes * 8 + bit + 1));
        }
        return mask;
    }
    static constexpr std::array<std::byte, bytes> encode(SignalMask mask)
    {
        std::array<std::byte, bytes> output{};
        for (usize word = 0; word < bytes / word_bytes; ++word)
        {
            u64 value{};
            for (usize bit = 0; bit < word_bytes * 8; ++bit)
                if (mask.contains(static_cast<u32>(word * word_bytes * 8 + bit + 1))) value |= u64{1} << bit;
            for (usize offset = 0; offset < word_bytes; ++offset)
                output[word * word_bytes + offset] = static_cast<std::byte>(value >>
                    (8 * (Abi::profile.big_endian ? word_bytes - 1 - offset : offset)));
        }
        return output;
    }
};

// Copy policies bind statically to the checked native user-copy boundary.
template <abi::linux_user::Profile Profile, typename Read, typename Write>
i64 update_signal_mask(SignalMask &current, i32 how, u64 input, u64 output, u64 size, Read &&read, Write &&write)
{
    using Codec = SignalMaskCodec<Profile>;
    if (size != Codec::bytes) return -22;
    const auto old = current;
    if (input)
    {
        std::array<std::byte, Codec::bytes> bytes{};
        if (!read(input, std::span<std::byte>{bytes})) return -14;
        const auto requested = Codec::sanitize(Codec::decode(bytes));
        if (how != Codec::block && how != Codec::unblock && how != Codec::replace) return -22;
        for (usize i = 0; i < current.words.size(); ++i)
            current.words[i] = how == Codec::block ? current.words[i] | requested.words[i] :
                how == Codec::unblock ? current.words[i] & ~requested.words[i] : requested.words[i];
        current = Codec::sanitize(current);
    }
    if (output)
    {
        const auto bytes = Codec::encode(old);
        if (!write(output, std::span<const std::byte>{bytes})) return -14;
    }
    return 0;
}

} // namespace ok::user::native
