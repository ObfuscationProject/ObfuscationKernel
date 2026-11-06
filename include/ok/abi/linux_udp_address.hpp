#pragma once

#include "ok/abi/linux_udp_data.hpp"
#include "ok/abi/linux_user.hpp"
#include "ok/net/address.hpp"
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

struct Ipv6SocketAddress
{
    u16 family{}, port{};
    u32 flowinfo{};
    net::wire::Ipv6 address{};
    u32 scope_id{};
};

// sockaddr_in6: family and scope_id in profile order, port and flowinfo in
// network order. A 24-byte RFC 2133 record without scope_id is input only.
template <Profile Selected>
    requires SupportedProfile<Selected>
class Ipv6SocketAddressCodec final
{
  public:
    static constexpr usize record_size = 28;
    static constexpr usize minimum_size = 24;
    static constexpr bool big_endian = Codec<Selected>::profile.big_endian;

    [[nodiscard]] static Result<Ipv6SocketAddress> decode(std::span<const u8> bytes)
    {
        if (bytes.size() != record_size && bytes.size() != minimum_size)
            return Status::invalid_argument("Linux IPv6 address record must be 24 or 28 bytes");
        Ipv6SocketAddress value{static_cast<u16>(big_endian ? (u16{bytes[0]} << 8) | bytes[1] : (u16{bytes[1]} << 8) | bytes[0]),
            static_cast<u16>((u16{bytes[2]} << 8) | bytes[3]), 0, {}, 0};
        for (usize at = 0; at < 4; ++at) value.flowinfo = (value.flowinfo << 8) | bytes[4 + at];
        for (usize at = 0; at < value.address.size(); ++at) value.address[at] = bytes[8 + at];
        if (bytes.size() == record_size)
            for (usize at = 0; at < 4; ++at)
                value.scope_id |= u32{bytes[24 + at]} << (big_endian ? (3 - at) * 8 : at * 8);
        return value;
    }

    [[nodiscard]] static Status encode(std::span<u8> bytes, Ipv6SocketAddress value)
    {
        if (bytes.size() != record_size)
            return Status::invalid_argument("Linux IPv6 address record must be twenty-eight bytes");
        bytes[0] = static_cast<u8>(big_endian ? value.family >> 8 : value.family);
        bytes[1] = static_cast<u8>(big_endian ? value.family : value.family >> 8);
        bytes[2] = static_cast<u8>(value.port >> 8);
        bytes[3] = static_cast<u8>(value.port);
        for (usize at = 0; at < 4; ++at) bytes[4 + at] = static_cast<u8>(value.flowinfo >> ((3 - at) * 8));
        for (usize at = 0; at < value.address.size(); ++at) bytes[8 + at] = value.address[at];
        for (usize at = 0; at < 4; ++at)
            bytes[24 + at] = static_cast<u8>(value.scope_id >> (big_endian ? (3 - at) * 8 : at * 8));
        return Status::success();
    }
};

// A copied output name of the socket's family: sockaddr_in or sockaddr_in6,
// with `size` the full Linux length callers report and truncate against.
struct SocketName
{
    std::array<u8, 28> bytes{};
    usize size{};
    [[nodiscard]] std::span<const u8> record() const { return {bytes.data(), size}; }
};

template <Profile Selected>
    requires SupportedProfile<Selected>
[[nodiscard]] Result<SocketName> encode_socket_name(const net::IpAddress &address, u16 port)
{
    SocketName name{};
    if (address.v4())
    {
        name.size = Ipv4SocketAddressCodec<Selected>::record_size;
        const Ipv4SocketAddress value{static_cast<u16>(udp_af_inet), port, address.ipv4()};
        if (auto status = Ipv4SocketAddressCodec<Selected>::encode(std::span{name.bytes}.first(name.size), value); !status)
            return status;
        return name;
    }
    // One interface: a link-local name reports it as scope 1, others scope 0.
    name.size = Ipv6SocketAddressCodec<Selected>::record_size;
    const Ipv6SocketAddress value{static_cast<u16>(udp_af_inet6), port, 0, address.ipv6(),
        net::wire::link_local(address.ipv6()) ? 1u : 0u};
    if (auto status = Ipv6SocketAddressCodec<Selected>::encode(name.bytes, value); !status) return status;
    return name;
}

} // namespace ok::abi::linux_user
