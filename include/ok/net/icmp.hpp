#pragma once

#include <ok/net/ip.hpp>
#include <optional>

namespace ok::net::wire
{
namespace icmpv4
{
inline constexpr u8 echo_reply = 0, unreachable = 3, echo_request = 8, time_exceeded = 11, parameter_problem = 12;
inline constexpr u8 net_unreachable = 0, host_unreachable = 1, protocol_unreachable = 2, port_unreachable = 3,
    fragmentation_needed = 4;
}
namespace icmpv6
{
inline constexpr u8 unreachable = 1, packet_too_big = 2, time_exceeded = 3, parameter_problem = 4;
inline constexpr u8 echo_request = 128, echo_reply = 129;
inline constexpr u8 router_solicitation = 133, router_advertisement = 134;
inline constexpr u8 neighbor_solicitation = 135, neighbor_advertisement = 136, redirect = 137;
inline constexpr u8 no_route = 0, address_unreachable = 3, port_unreachable = 4;
}

// `rest` is the second header word (echo identifier/sequence, MTU, pointer);
// `body` follows the eight-byte header and borrows the caller's packet.
struct Icmp
{
    u8 type{}, code{};
    u32 rest{};
    Bytes message{}, body{};
    [[nodiscard]] u16 identifier() const { return static_cast<u16>(rest >> 16); }
    [[nodiscard]] u16 sequence() const { return static_cast<u16>(rest); }
};

template <AddressFamily Family>
inline Result<Icmp> icmp(const typename Family::Packet &ip)
{
    if (ip.protocol != Family::icmp_protocol) return Status::unsupported("IP payload is not ICMP");
    if (ip.payload.size() < 8) return Status::invalid_argument("truncated ICMP header");
    if (sum(ip.payload, Family::icmp_initial(ip.source, ip.destination, ip.payload.size())) != 0xffffu)
        return Status::invalid_argument("invalid ICMP checksum");
    Icmp view; view.type = byte(ip.payload, 0); view.code = byte(ip.payload, 1);
    view.rest = tcp_detail::be32(ip.payload, 4); view.message = ip.payload; view.body = ip.payload.subspan(8);
    return view;
}

// ICMPv4 error and informational types; an ICMPv6 type below 128 is an error.
template <AddressFamily Family>
inline constexpr bool icmp_error(u8 type)
{
    if constexpr (Family::version == 6) return type < 128;
    else return type == icmpv4::unreachable || type == icmpv4::time_exceeded || type == icmpv4::parameter_problem ||
        type == 4 || type == 5;
}

template <AddressFamily Family>
inline Result<usize> build_icmp(std::span<std::byte> output, const typename Family::Address &source,
                                const typename Family::Address &destination, u8 ttl, u16 identification,
                                u8 type, u8 code, u32 rest, Bytes body)
{
    constexpr usize header = Family::header_size;
    if (!ttl) return Status::invalid_argument("ICMP send TTL must be nonzero");
    if (body.size() > Family::max_packet - header - 8 || body.size() > 65535 - 8 || output.size() < header + 8 + body.size())
        return Status::overflow("ICMP packet exceeds output capacity");
    const usize length = header + 8 + body.size();
    if (tcp_detail::overlaps(body, output.first(length))) return Status::invalid_argument("ICMP input aliases output");
    Family::header(output, source, destination, Family::icmp_protocol, length - header, ttl, identification);
    output[header] = std::byte{type}; output[header + 1] = std::byte{code}; be16(output, header + 2, 0);
    tcp_detail::be32(output, header + 4, rest);
    ip_detail::copy(output, header + 8, body);
    be16(output, header + 2, static_cast<u16>(~sum(Bytes{output.data() + header, length - header},
        Family::icmp_initial(source, destination, length - header))));
    return length;
}

// Quotes as much of the offending packet as the error may carry: 576 bytes in
// all for IPv4 (RFC 1812 section 4.3.2.3) and the 1280-byte IPv6 minimum MTU
// (RFC 4443 section 2.4). Whether an error may be sent is the caller's policy.
template <AddressFamily Family>
inline Result<usize> build_icmp_error(std::span<std::byte> output, const typename Family::Address &source,
                                      const typename Family::Address &destination, u16 identification,
                                      u8 type, u8 code, u32 rest, Bytes offending)
{
    constexpr usize limit = Family::version == 6 ? 1280 : 576;
    if (!icmp_error<Family>(type)) return Status::invalid_argument("ICMP type is not an error");
    const usize room = limit - Family::header_size - 8;
    return build_icmp<Family>(output, source, destination, 64, identification,
                              type, code, rest, offending.first(offending.size() < room ? offending.size() : room));
}

// Neighbour discovery (RFC 4861) over Ethernet.
struct PrefixInformation
{
    Ipv6 prefix{};
    u8 length{};
    bool on_link{}, autonomous{};
    u32 valid_lifetime{}, preferred_lifetime{};
};

enum class NdKind : u8 { router_solicitation, router_advertisement, neighbor_solicitation, neighbor_advertisement };
struct NdMessage
{
    static constexpr usize max_prefixes = 4;
    NdKind kind{};
    Ipv6 target{};
    bool router{}, solicited{}, override_flag{}, managed{}, other{};
    u8 hop_limit{};
    u16 router_lifetime{};
    u32 reachable_time{}, retransmit_timer{};
    std::optional<Mac> source_link{}, target_link{};
    std::optional<u32> mtu{};
    std::array<PrefixInformation, max_prefixes> prefixes{};
    usize prefix_count{}, prefixes_ignored{};
};

namespace nd_detail
{
inline Status options(Bytes options, NdMessage &message)
{
    for (usize at = 0; at < options.size();)
    {
        if (options.size() - at < 2) return Status::invalid_argument("truncated ND option");
        const u8 type = byte(options, at);
        const usize length = usize{byte(options, at + 1)} * 8u;
        if (!length || length > options.size() - at) return Status::invalid_argument("invalid ND option length");
        const Bytes option = options.subspan(at, length);
        if (type == 1 || type == 2)
        {
            if (length != 8) return Status::invalid_argument("ND link-layer option is not Ethernet");
            Mac mac{}; for (usize i = 0; i < 6; ++i) mac[i] = byte(option, 2 + i);
            (type == 1 ? message.source_link : message.target_link) = mac;
        }
        else if (type == 3)
        {
            if (length != 32) return Status::invalid_argument("ND prefix information length is invalid");
            PrefixInformation prefix{};
            prefix.length = byte(option, 2);
            prefix.on_link = byte(option, 3) & 0x80u; prefix.autonomous = byte(option, 3) & 0x40u;
            prefix.valid_lifetime = tcp_detail::be32(option, 4); prefix.preferred_lifetime = tcp_detail::be32(option, 8);
            for (usize i = 0; i < 16; ++i) prefix.prefix[i] = byte(option, 16 + i);
            // RFC 4862 section 5.5.3 ignores an option whose preferred lifetime
            // exceeds its valid lifetime; link-local prefixes are not on-link data.
            if (prefix.length > 128 || prefix.preferred_lifetime > prefix.valid_lifetime || link_local(prefix.prefix) ||
                message.prefix_count == NdMessage::max_prefixes)
                ++message.prefixes_ignored;
            else message.prefixes[message.prefix_count++] = prefix;
        }
        else if (type == 5)
        {
            if (length != 8) return Status::invalid_argument("ND MTU option length is invalid");
            message.mtu = tcp_detail::be32(option, 4);
        }
        at += length;
    }
    return Status::success();
}
}

// Validity checks of RFC 4861 sections 6.1 and 7.1. Options a message type
// does not use are parsed for shape and otherwise ignored.
inline Result<NdMessage> neighbor_discovery(const Ipv6Packet &ip, const Icmp &message)
{
    using namespace icmpv6;
    if (message.type < router_solicitation || message.type > neighbor_advertisement)
        return Status::unsupported("ICMPv6 message is not neighbour discovery");
    if (ip.hop_limit != 255 || message.code) return Status::invalid_argument("ND message hop limit or code is invalid");
    NdMessage view;
    const Bytes bytes = message.message;
    usize fixed = 8;
    if (message.type == router_solicitation) view.kind = NdKind::router_solicitation;
    else if (message.type == router_advertisement)
    {
        if (!link_local(ip.source)) return Status::invalid_argument("router advertisement source is not link-local");
        if (bytes.size() < 16) return Status::invalid_argument("truncated router advertisement");
        view.kind = NdKind::router_advertisement; fixed = 16;
        view.hop_limit = byte(bytes, 4); view.managed = byte(bytes, 5) & 0x80u; view.other = byte(bytes, 5) & 0x40u;
        view.router_lifetime = be16(bytes, 6);
        view.reachable_time = tcp_detail::be32(bytes, 8); view.retransmit_timer = tcp_detail::be32(bytes, 12);
    }
    else
    {
        if (bytes.size() < 24) return Status::invalid_argument("truncated neighbour message");
        fixed = 24;
        for (usize i = 0; i < 16; ++i) view.target[i] = byte(bytes, 8 + i);
        if (multicast(view.target)) return Status::invalid_argument("neighbour target is multicast");
        if (message.type == neighbor_solicitation) view.kind = NdKind::neighbor_solicitation;
        else
        {
            view.kind = NdKind::neighbor_advertisement;
            const u8 flags = byte(bytes, 4);
            view.router = flags & 0x80u; view.solicited = flags & 0x40u; view.override_flag = flags & 0x20u;
            if (view.solicited && multicast(ip.destination))
                return Status::invalid_argument("solicited advertisement sent to multicast");
        }
    }
    if (auto status = nd_detail::options(bytes.subspan(fixed), view); !status) return status;
    if (unspecified(ip.source))
    {
        if (view.source_link) return Status::invalid_argument("unspecified ND source carries a link-layer address");
        if (view.kind == NdKind::neighbor_solicitation && ip.destination != solicited_node(view.target))
            return Status::invalid_argument("duplicate address probe is not sent to the solicited-node group");
        if (view.kind == NdKind::neighbor_advertisement || view.kind == NdKind::router_advertisement)
            return Status::invalid_argument("advertisement source is unspecified");
    }
    return view;
}

namespace nd_detail
{
inline Result<usize> build(std::span<std::byte> output, const Ipv6 &source, const Ipv6 &destination, u8 type,
                           u32 rest, const Ipv6 *target, u8 option, std::optional<Mac> link)
{
    std::array<std::byte, 24> body{};
    usize size = 0;
    if (target) { for (usize i = 0; i < 16; ++i) body[i] = std::byte{(*target)[i]}; size = 16; }
    if (link)
    {
        body[size] = std::byte{option}; body[size + 1] = std::byte{1};
        for (usize i = 0; i < 6; ++i) body[size + 2 + i] = std::byte{(*link)[i]};
        size += 8;
    }
    return build_icmp<Ipv6Family>(output, source, destination, 255, 0, type, 0, rest, Bytes{body.data(), size});
}
}

inline Result<usize> build_neighbor_solicitation(std::span<std::byte> output, const Ipv6 &source,
                                                 const Ipv6 &destination, const Ipv6 &target, std::optional<Mac> link)
{
    if (multicast(target)) return Status::invalid_argument("neighbour target is multicast");
    if (unspecified(source) && link) return Status::invalid_argument("duplicate address probe carries a link-layer address");
    return nd_detail::build(output, source, destination, icmpv6::neighbor_solicitation, 0, &target, 1, link);
}

inline Result<usize> build_neighbor_advertisement(std::span<std::byte> output, const Ipv6 &source,
                                                  const Ipv6 &destination, const Ipv6 &target, bool router,
                                                  bool solicited, bool override_flag, std::optional<Mac> link)
{
    if (multicast(target) || unspecified(source)) return Status::invalid_argument("invalid neighbour advertisement addresses");
    if (solicited && multicast(destination)) return Status::invalid_argument("solicited advertisement sent to multicast");
    const u32 flags = (router ? 0x80000000u : 0) | (solicited ? 0x40000000u : 0) | (override_flag ? 0x20000000u : 0);
    return nd_detail::build(output, source, destination, icmpv6::neighbor_advertisement, flags, &target, 2, link);
}

inline Result<usize> build_router_solicitation(std::span<std::byte> output, const Ipv6 &source, std::optional<Mac> link)
{
    if (unspecified(source) && link) return Status::invalid_argument("unspecified solicitation carries a link-layer address");
    return nd_detail::build(output, source, all_routers, icmpv6::router_solicitation, 0, nullptr, 1, link);
}
}
