#pragma once

#include <ok/net/tcp_wire.hpp>
#include <concepts>

namespace ok::net::wire
{
using Ipv6 = std::array<u8, 16>;

inline constexpr u16 ethertype_ipv4 = 0x0800, ethertype_arp = 0x0806, ethertype_ipv6 = 0x86dd;
inline constexpr u8 protocol_icmp = 1, protocol_tcp = 6, protocol_udp = 17, protocol_icmpv6 = 58;
inline constexpr u8 protocol_no_next = 59;
inline constexpr Mac broadcast_mac{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
inline constexpr Ipv6 all_nodes{0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
inline constexpr Ipv6 all_routers{0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

inline constexpr bool multicast(Mac mac) { return mac[0] & 1u; }
inline constexpr bool multicast(Ipv4 address) { return address[0] >= 224 && address[0] < 240; }
inline constexpr bool broadcast(Ipv4 address) { return address == Ipv4{255, 255, 255, 255}; }
inline constexpr bool unspecified(Ipv4 address) { return address == Ipv4{}; }
inline constexpr bool loopback(Ipv4 address) { return address[0] == 127; }
inline constexpr bool multicast(const Ipv6 &address) { return address[0] == 0xff; }
inline constexpr bool unspecified(const Ipv6 &address) { return address == Ipv6{}; }
inline constexpr bool loopback(const Ipv6 &address)
{ Ipv6 one{}; one[15] = 1; return address == one; }
inline constexpr bool link_local(const Ipv6 &address) { return address[0] == 0xfe && (address[1] & 0xc0u) == 0x80; }
// IPv4-mapped addresses never appear on the wire (RFC 4291 section 2.5.5.2).
inline constexpr bool ipv4_mapped(const Ipv6 &address)
{
    for (usize i = 0; i < 10; ++i) if (address[i]) return false;
    return address[10] == 0xff && address[11] == 0xff;
}
inline constexpr Ipv6 solicited_node(const Ipv6 &target)
{ return {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff, target[13], target[14], target[15]}; }
inline constexpr Mac multicast_mac(const Ipv6 &address)
{ return {0x33, 0x33, address[12], address[13], address[14], address[15]}; }
// Modified EUI-64 interface identifier (RFC 4291 appendix A).
inline constexpr Ipv6 with_interface_id(Ipv6 prefix, Mac mac)
{
    prefix[8] = static_cast<u8>(mac[0] ^ 2u); prefix[9] = mac[1]; prefix[10] = mac[2]; prefix[11] = 0xff;
    prefix[12] = 0xfe; prefix[13] = mac[3]; prefix[14] = mac[4]; prefix[15] = mac[5];
    return prefix;
}
inline constexpr Ipv6 link_local_from_mac(Mac mac) { return with_interface_id({0xfe, 0x80}, mac); }

// Prefix comparison of the first `length` bits; length must not exceed the address width.
template <usize Size>
inline constexpr bool prefix_match(const std::array<u8, Size> &a, const std::array<u8, Size> &b, u8 length)
{
    const usize bytes = length / 8u, bits = length % 8u;
    for (usize i = 0; i < bytes; ++i) if (a[i] != b[i]) return false;
    if (!bits) return true;
    const u8 mask = static_cast<u8>(0xffu << (8u - bits));
    return (a[bytes] & mask) == (b[bytes] & mask);
}

struct Ipv6Packet
{
    Ipv6 source{}, destination{};
    u8 traffic_class{}, hop_limit{}, protocol{};
    u32 flow_label{};
    Bytes payload{};
};

namespace ip_detail
{
inline Status options(Bytes header)
{
    for (usize at = 2; at < header.size();)
    {
        const u8 type = byte(header, at);
        if (type == 0) { ++at; continue; }
        if (header.size() - at < 2) return Status::invalid_argument("truncated IPv6 option");
        const usize length = 2u + byte(header, at + 1);
        if (length > header.size() - at) return Status::invalid_argument("invalid IPv6 option length");
        // Action bits other than "skip" ask for a discard or a parameter
        // problem report; neither is processed by this profile.
        if (type != 1 && (type >> 6)) return Status::unsupported("IPv6 option requires processing outside this profile");
        at += length;
    }
    return Status::success();
}
inline void copy(std::span<std::byte> output, usize at, Bytes input)
{ for (usize i = 0; i < input.size(); ++i) output[at + i] = input[i]; }
}

// Hop-by-hop, destination-options and exhausted routing headers are skipped;
// `protocol` names the upper layer and `payload` starts after the last
// extension header. Fragments stay unsupported, as for IPv4.
inline Result<Ipv6Packet> ipv6(Bytes packet)
{
    if (packet.size() < 40) return Status::invalid_argument("truncated IPv6 header");
    if ((byte(packet, 0) >> 4) != 6) return Status::unsupported("packet is not IPv6");
    const usize length = be16(packet, 4);
    if (40 + length > packet.size()) return Status::invalid_argument("invalid IPv6 payload length");
    Ipv6Packet view;
    for (usize i = 0; i < 16; ++i) { view.source[i] = byte(packet, 8 + i); view.destination[i] = byte(packet, 24 + i); }
    if (multicast(view.source)) return Status::invalid_argument("IPv6 source address is multicast");
    view.traffic_class = static_cast<u8>(((byte(packet, 0) & 15u) << 4) | (byte(packet, 1) >> 4));
    view.flow_label = (u32{byte(packet, 1) & 15u} << 16) | be16(packet, 2);
    view.hop_limit = byte(packet, 7);
    u8 next = byte(packet, 6);
    const Bytes payload = packet.subspan(40, length);
    usize at = 0;
    for (bool first = true;; first = false)
    {
        if (next == 0 && !first) return Status::invalid_argument("IPv6 hop-by-hop header must follow the fixed header");
        if (next != 0 && next != 43 && next != 60)
        {
            if (next == 44) return Status::unsupported("IPv6 fragment reassembly is not yet available");
            break;
        }
        if (payload.size() - at < 8) return Status::invalid_argument("truncated IPv6 extension header");
        const usize size = (usize{byte(payload, at + 1)} + 1u) * 8u;
        if (size > payload.size() - at) return Status::invalid_argument("invalid IPv6 extension header length");
        const Bytes header = payload.subspan(at, size);
        if (next == 43)
        {
            if (byte(header, 3)) return Status::unsupported("IPv6 routing header has segments left");
        }
        else if (auto status = ip_detail::options(header); !status) return status;
        next = byte(header, 0); at += size;
    }
    view.protocol = next; view.payload = payload.subspan(at);
    return view;
}

inline u32 pseudo_sum(const Ipv6 &source, const Ipv6 &destination, u8 protocol, u32 length)
{
    u32 value = protocol + (length >> 16) + (length & 0xffffu);
    for (usize i = 0; i < 16; i += 2)
    {
        value += (u32{source[i]} << 8) | source[i + 1];
        value += (u32{destination[i]} << 8) | destination[i + 1];
        value = (value & 0xffffu) + (value >> 16);
    }
    return (value & 0xffffu) + (value >> 16);
}

// Static selection of the network layer. Generic transport codecs below take
// the family as an explicit template argument; the IPv4 overloads in wire.hpp
// and tcp_wire.hpp keep their exact behavior and output bytes.
template <class Family>
concept AddressFamily = requires(Bytes bytes, const typename Family::Address &address, std::span<std::byte> output,
                                 u8 protocol, usize length, u8 ttl, u16 identification)
{
    typename Family::Packet;
    { Family::version } -> std::convertible_to<u8>;
    { Family::ethertype } -> std::convertible_to<u16>;
    { Family::header_size } -> std::convertible_to<usize>;
    { Family::icmp_protocol } -> std::convertible_to<u8>;
    { Family::max_packet } -> std::convertible_to<usize>;
    { Family::parse(bytes) } -> std::same_as<Result<typename Family::Packet>>;
    { Family::pseudo(address, address, protocol, length) } -> std::same_as<u32>;
    { Family::icmp_initial(address, address, length) } -> std::same_as<u32>;
    { Family::header(output, address, address, protocol, length, ttl, identification) } -> std::same_as<void>;
} && requires(const typename Family::Packet &packet)
{
    { packet.source } -> std::convertible_to<typename Family::Address>;
    { packet.destination } -> std::convertible_to<typename Family::Address>;
    { packet.protocol } -> std::convertible_to<u8>;
    { packet.payload } -> std::convertible_to<Bytes>;
};

struct Ipv4Family
{
    using Address = Ipv4;
    using Packet = Ipv4Packet;
    static constexpr u8 version = 4, icmp_protocol = protocol_icmp;
    static constexpr u16 ethertype = ethertype_ipv4;
    static constexpr usize header_size = 20, max_packet = 65535;
    static Result<Packet> parse(Bytes bytes) { return ipv4(bytes); }
    static u32 pseudo(const Address &source, const Address &destination, u8 protocol, usize length)
    { return pseudo_sum(source, destination, protocol, static_cast<u16>(length)); }
    static u32 icmp_initial(const Address &, const Address &, usize) { return 0; }
    // `length` is the upper-layer length; the caller checked the 64 KiB bound.
    static void header(std::span<std::byte> output, const Address &source, const Address &destination,
                       u8 protocol, usize length, u8 ttl, u16 identification)
    {
        for (usize i = 0; i < 20; ++i) output[i] = std::byte{};
        output[0] = std::byte{0x45}; output[8] = std::byte{ttl}; output[9] = std::byte{protocol};
        be16(output, 2, static_cast<u16>(20 + length)); be16(output, 4, identification); be16(output, 6, 0x4000);
        for (usize i = 0; i < 4; ++i) { output[12 + i] = std::byte{source[i]}; output[16 + i] = std::byte{destination[i]}; }
        be16(output, 10, checksum(Bytes{output.data(), 20}));
    }
};

struct Ipv6Family
{
    using Address = Ipv6;
    using Packet = Ipv6Packet;
    static constexpr u8 version = 6, icmp_protocol = protocol_icmpv6;
    static constexpr u16 ethertype = ethertype_ipv6;
    static constexpr usize header_size = 40, max_packet = 40 + 65535;
    static Result<Packet> parse(Bytes bytes) { return ipv6(bytes); }
    static u32 pseudo(const Address &source, const Address &destination, u8 protocol, usize length)
    { return pseudo_sum(source, destination, protocol, static_cast<u32>(length)); }
    static u32 icmp_initial(const Address &source, const Address &destination, usize length)
    { return pseudo(source, destination, protocol_icmpv6, length); }
    // IPv6 has no header identification; fragments are never emitted.
    static void header(std::span<std::byte> output, const Address &source, const Address &destination,
                       u8 protocol, usize length, u8 hop_limit, u16)
    {
        for (usize i = 0; i < 40; ++i) output[i] = std::byte{};
        output[0] = std::byte{0x60}; be16(output, 4, static_cast<u16>(length));
        output[6] = std::byte{protocol}; output[7] = std::byte{hop_limit};
        for (usize i = 0; i < 16; ++i) { output[8 + i] = std::byte{source[i]}; output[24 + i] = std::byte{destination[i]}; }
    }
};
static_assert(AddressFamily<Ipv4Family> && AddressFamily<Ipv6Family>);

// Version dispatch for a received network-layer packet.
inline constexpr u8 ip_version(Bytes packet) { return packet.empty() ? u8{} : static_cast<u8>(byte(packet, 0) >> 4); }

template <AddressFamily Family>
inline Result<Udp> udp(const typename Family::Packet &ip)
{
    if (ip.protocol != protocol_udp) return Status::unsupported("IP payload is not UDP");
    if (ip.payload.size() < 8) return Status::invalid_argument("truncated UDP header");
    const usize length = be16(ip.payload, 4);
    if (length < 8 || length > ip.payload.size()) return Status::invalid_argument("invalid UDP length");
    const auto datagram = ip.payload.first(length);
    Udp view; view.source_port = be16(datagram, 0); view.destination_port = be16(datagram, 2);
    if (!view.destination_port) return Status::invalid_argument("UDP destination port zero is reserved");
    view.checksum_present = be16(datagram, 6) != 0;
    // RFC 8200 section 8.1: IPv6 receivers discard a zero UDP checksum.
    if (!view.checksum_present && Family::version == 6) return Status::invalid_argument("IPv6 UDP checksum is mandatory");
    if (view.checksum_present && sum(datagram, Family::pseudo(ip.source, ip.destination, protocol_udp, length)) != 0xffffu)
        return Status::invalid_argument("invalid UDP pseudo-header checksum");
    view.payload = datagram.subspan(8);
    return view;
}

template <AddressFamily Family>
inline Result<Tcp> tcp(const typename Family::Packet &ip)
{
    if (ip.protocol != protocol_tcp) return Status::unsupported("IP payload is not TCP");
    const auto segment = ip.payload;
    if (segment.size() < 20 || segment.size() > Family::max_packet - Family::header_size)
        return Status::invalid_argument("invalid TCP segment length");
    const usize header = (byte(segment, 12) >> 4) * 4u;
    if (header < 20 || header > segment.size()) return Status::invalid_argument("invalid TCP header length");
    if (sum(segment, Family::pseudo(ip.source, ip.destination, protocol_tcp, segment.size())) != 0xffffu)
        return Status::invalid_argument("invalid TCP pseudo-header checksum");
    const u8 flags = byte(segment, 13);
    const auto options = segment.subspan(20, header - 20);
    if (auto status = tcp_detail::options(options, flags); !status) return status;
    return Tcp{TcpFields{be16(segment, 0), be16(segment, 2), tcp_detail::be32(segment, 4),
        tcp_detail::be32(segment, 8), flags, be16(segment, 14), be16(segment, 18)},
        options, segment.subspan(header)};
}

// Builders share the IPv4 contract: payload and options must not overlap the
// emitted bytes, and an invalid call leaves the whole output unchanged.
template <AddressFamily Family>
inline Result<usize> build_udp(std::span<std::byte> output, const typename Family::Address &source,
                               const typename Family::Address &destination, u16 source_port, u16 destination_port,
                               u16 identification, u8 ttl, Bytes payload)
{
    constexpr usize header = Family::header_size;
    if (!ttl) return Status::invalid_argument("UDP send TTL must be nonzero");
    if (payload.size() > Family::max_packet - header - 8 || payload.size() > 65535 - 8 ||
        output.size() < header + 8 + payload.size())
        return Status::overflow("UDP packet exceeds output capacity");
    const usize length = header + 8 + payload.size();
    if (tcp_detail::overlaps(payload, output.first(length))) return Status::invalid_argument("UDP input aliases output");
    Family::header(output, source, destination, protocol_udp, length - header, ttl, identification);
    be16(output, header, source_port); be16(output, header + 2, destination_port);
    be16(output, header + 4, static_cast<u16>(length - header)); be16(output, header + 6, 0);
    ip_detail::copy(output, header + 8, payload);
    u16 value = static_cast<u16>(~sum(Bytes{output.data() + header, length - header},
        Family::pseudo(source, destination, protocol_udp, length - header)));
    if (!value) value = 0xffff;
    be16(output, header + 6, value);
    return length;
}

template <AddressFamily Family>
inline Result<usize> build_tcp(std::span<std::byte> output, const typename Family::Address &source,
                               const typename Family::Address &destination, u16 identification, u8 ttl,
                               TcpFields fields, Bytes options, Bytes payload)
{
    constexpr usize ip = Family::header_size;
    if (!ttl) return Status::invalid_argument("TCP send TTL must be nonzero");
    if (auto status = tcp_detail::options(options, fields.flags); !status) return status;
    const usize header = 20 + ((options.size() + 3) & ~usize{3});
    if (payload.size() > Family::max_packet - ip - header) return Status::overflow("TCP payload exceeds packet limit");
    const usize length = ip + header + payload.size();
    if (output.size() < length) return Status::overflow("TCP packet exceeds output capacity");
    const auto emitted = output.first(length);
    if (tcp_detail::overlaps(options, emitted) || tcp_detail::overlaps(payload, emitted))
        return Status::invalid_argument("TCP input aliases output");
    Family::header(output, source, destination, protocol_tcp, length - ip, ttl, identification);
    for (usize i = ip; i < ip + header; ++i) output[i] = std::byte{};
    be16(output, ip, fields.source_port); be16(output, ip + 2, fields.destination_port);
    tcp_detail::be32(output, ip + 4, fields.sequence); tcp_detail::be32(output, ip + 8, fields.acknowledgment);
    output[ip + 12] = std::byte{static_cast<u8>((header / 4) << 4)}; output[ip + 13] = std::byte{fields.flags};
    be16(output, ip + 14, fields.window); be16(output, ip + 18, fields.urgent_pointer);
    ip_detail::copy(output, ip + 20, options);
    ip_detail::copy(output, ip + header, payload);
    be16(output, ip + 16, static_cast<u16>(~sum(Bytes{output.data() + ip, length - ip},
        Family::pseudo(source, destination, protocol_tcp, length - ip))));
    return length;
}

// Writes the 14-byte header; the caller places `payload` bytes at offset 14.
inline Result<usize> build_ethernet(std::span<std::byte> output, Mac destination, Mac source, u16 type, usize payload)
{
    if (type < 0x0600) return Status::invalid_argument("Ethernet type must not be an 802.3 length");
    if (payload > 1500 || output.size() < 14 + payload) return Status::overflow("Ethernet frame exceeds output capacity");
    for (usize i = 0; i < 6; ++i) { output[i] = std::byte{destination[i]}; output[6 + i] = std::byte{source[i]}; }
    be16(output, 12, type);
    return 14 + payload;
}

inline Result<usize> build_arp(std::span<std::byte> output, u16 operation, Mac sender_mac, Ipv4 sender_ip,
                               Mac target_mac, Ipv4 target_ip)
{
    if (operation != 1 && operation != 2) return Status::invalid_argument("ARP operation is not request or reply");
    if (output.size() < 28) return Status::overflow("ARP packet exceeds output capacity");
    be16(output, 0, 1); be16(output, 2, ethertype_ipv4); output[4] = std::byte{6}; output[5] = std::byte{4};
    be16(output, 6, operation);
    for (usize i = 0; i < 6; ++i) { output[8 + i] = std::byte{sender_mac[i]}; output[18 + i] = std::byte{target_mac[i]}; }
    for (usize i = 0; i < 4; ++i) { output[14 + i] = std::byte{sender_ip[i]}; output[24 + i] = std::byte{target_ip[i]}; }
    return usize{28};
}
}
