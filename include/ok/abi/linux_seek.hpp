#pragma once
#include "ok/abi/linux_user.hpp"

namespace ok::abi::linux_user
{
// _llseek uses two unsigned long scalar halves, not an ABI-aligned off64 pair.
// Its result is an eight-byte target-endian loff_t, even on 32-bit profiles.
template <Profile Selected>
    requires SupportedProfile<Selected>
class SeekCodec final
{
  public:
    static i64 word_offset(u64 value)
    {
        if constexpr (Codec<Selected>::profile.word_bytes == 4)
            return static_cast<i32>(static_cast<u32>(value));
        return static_cast<i64>(value);
    }
    static i64 wide_offset(u64 high, u64 low)
    {
        return static_cast<i64>((static_cast<u64>(static_cast<u32>(high)) << 32) |
                                static_cast<u32>(low));
    }
    static std::array<u8, 8> encode(u64 value)
    {
        std::array<u8, 8> bytes{};
        for (usize byte = 0; byte < bytes.size(); ++byte)
        {
            const auto shift = Codec<Selected>::profile.big_endian ? 7 - byte : byte;
            bytes[byte] = static_cast<u8>(value >> (8 * shift));
        }
        return bytes;
    }
};
} // namespace ok::abi::linux_user
