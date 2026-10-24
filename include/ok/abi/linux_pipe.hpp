#pragma once
#include "ok/abi/linux_open.hpp"

namespace ok::abi::linux_user
{
// Linux pipe2 always publishes two target-endian int32 descriptors. Only the
// old MIPS pipe syscall publishes its pair through v0/v1; pipe2 never does.
template <Profile Selected>
    requires SupportedProfile<Selected>
class PipeCodec final
{
  public:
    static constexpr bool register_pair = Selected == Profile::mips || Selected == Profile::mips64;
    static Result<u32> decode_flags(u32 flags)
    {
        using Flags = OpenCodec<Selected>;
        if (flags & ~(Flags::nonblock | Flags::cloexec))
            return Status::invalid_argument("unsupported Linux pipe flags");
        return ((flags & Flags::nonblock) ? posix::o_NONBLOCK : 0u) |
               ((flags & Flags::cloexec) ? posix::o_CLOEXEC : 0u);
    }
    static std::array<u8, 8> encode(const std::array<i32, 2> &descriptors)
    {
        std::array<u8, 8> bytes{};
        for (usize fd = 0; fd < 2; ++fd)
            for (usize byte = 0; byte < 4; ++byte)
            {
                const auto shift = Codec<Selected>::profile.big_endian ? 3 - byte : byte;
                bytes[4 * fd + byte] = static_cast<u8>(static_cast<u32>(descriptors[fd]) >> (8 * shift));
            }
        return bytes;
    }
};
} // namespace ok::abi::linux_user
