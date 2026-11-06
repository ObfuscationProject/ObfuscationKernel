#pragma once

#include <ok/net/ip.hpp>

namespace ok::net
{
enum class Family : u8 { ipv4 = 4, ipv6 = 6 };

// A transport endpoint address of either family. IPv4 keeps its octets in
// bytes[0..4) and zeros elsewhere, so a default value is 0.0.0.0. Families
// never compare equal; IPv4-mapped IPv6 addresses are not translated.
struct IpAddress
{
    Family family{Family::ipv4};
    std::array<u8, 16> bytes{};

    constexpr IpAddress() = default;
    constexpr IpAddress(const wire::Ipv4 &address) { for (usize i = 0; i < 4; ++i) bytes[i] = address[i]; }
    constexpr IpAddress(const wire::Ipv6 &address) : family{Family::ipv6}, bytes{address} {}
    constexpr IpAddress(u8 a, u8 b, u8 c, u8 d) : bytes{a, b, c, d} {}

    [[nodiscard]] constexpr bool v4() const { return family == Family::ipv4; }
    [[nodiscard]] constexpr bool v6() const { return family == Family::ipv6; }
    [[nodiscard]] constexpr wire::Ipv4 ipv4() const { return {bytes[0], bytes[1], bytes[2], bytes[3]}; }
    [[nodiscard]] constexpr wire::Ipv6 ipv6() const { return bytes; }
    [[nodiscard]] constexpr u8 operator[](usize index) const { return bytes[index]; }
    [[nodiscard]] constexpr bool unspecified() const { return v4() ? wire::unspecified(ipv4()) : wire::unspecified(ipv6()); }
    // A unicast address a transport may use as either end of a connection.
    [[nodiscard]] constexpr bool unicast() const
    {
        if (v4()) return bytes[0] && bytes[0] < 224;
        const auto a = ipv6();
        return !wire::unspecified(a) && !wire::multicast(a) && !wire::loopback(a) && !wire::ipv4_mapped(a);
    }
    [[nodiscard]] constexpr bool operator==(const IpAddress &) const = default;
    [[nodiscard]] constexpr bool operator==(const wire::Ipv4 &address) const { return v4() && ipv4() == address; }
    [[nodiscard]] constexpr bool operator==(const wire::Ipv6 &address) const { return v6() && bytes == address; }
};
}
