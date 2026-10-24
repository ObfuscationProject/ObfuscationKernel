#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/net/wire.hpp"

namespace ok::abi::linux_user
{

// Copied record fields only. Family admission belongs to each socket operation.
struct Ipv4SocketAddress
{
    u16 family{}, port{};
    net::wire::Ipv4 address{};
};

template <Profile Selected>
    requires SupportedProfile<Selected>
class Ipv4SocketAddressCodec final
{
  public:
    static constexpr usize record_size = 16;
    static constexpr bool big_endian = Codec<Selected>::profile.big_endian;

    [[nodiscard]] static Result<Ipv4SocketAddress> decode(std::span<const u8> bytes)
    {
        if (bytes.size() != record_size)
            return Status::invalid_argument("Linux IPv4 address record must be sixteen bytes");
        const u16 family = big_endian
            ? static_cast<u16>((u16{bytes[0]} << 8) | bytes[1])
            : static_cast<u16>((u16{bytes[1]} << 8) | bytes[0]);
        return Ipv4SocketAddress{family,
            static_cast<u16>((u16{bytes[2]} << 8) | bytes[3]),
            {bytes[4], bytes[5], bytes[6], bytes[7]}};
    }

    [[nodiscard]] static Status encode(std::span<u8> bytes, Ipv4SocketAddress value)
    {
        if (bytes.size() != record_size)
            return Status::invalid_argument("Linux IPv4 address record must be sixteen bytes");
        bytes[0] = static_cast<u8>(big_endian ? value.family >> 8 : value.family);
        bytes[1] = static_cast<u8>(big_endian ? value.family : value.family >> 8);
        bytes[2] = static_cast<u8>(value.port >> 8);
        bytes[3] = static_cast<u8>(value.port);
        for (usize at = 0; at < value.address.size(); ++at) bytes[at + 4] = value.address[at];
        for (usize at = 8; at < record_size; ++at) bytes[at] = 0;
        return Status::success();
    }
};

} // namespace ok::abi::linux_user
