#pragma once

#include <ok/core/types.hpp>
#include <array>
#include <span>

namespace ok::net::wire
{
using Bytes = std::span<const std::byte>;
using Mac = std::array<u8, 6>;
using Ipv4 = std::array<u8, 4>;

// Unchecked field helpers require a span covering the requested field. The
// public packet parsers below check lengths before calling them.
inline u8 byte(Bytes bytes, usize at) { return std::to_integer<u8>(bytes[at]); }
inline u16 be16(Bytes bytes, usize at) { return static_cast<u16>((u16{byte(bytes, at)} << 8) | byte(bytes, at + 1)); }
inline void be16(std::span<std::byte> bytes, usize at, u16 value)
{
    bytes[at] = std::byte{static_cast<u8>(value >> 8)};
    bytes[at + 1] = std::byte{static_cast<u8>(value)};
}
inline u32 sum(Bytes bytes, u32 initial = 0)
{
    u32 value = (initial & 0xffffu) + (initial >> 16);
    for (usize at = 0; at < bytes.size(); at += 2)
    {
        const u16 word = static_cast<u16>((u16{byte(bytes, at)} << 8) |
            (at + 1 < bytes.size() ? byte(bytes, at + 1) : 0));
        value += word;
        value = (value & 0xffffu) + (value >> 16);
    }
    while (value >> 16) value = (value & 0xffffu) + (value >> 16);
    return value;
}
// A complete buffer is required: sum does not carry odd-byte parity between
// chunks. An initial value is an accumulated sum, not an arbitrary byte stream.
inline u16 checksum(Bytes bytes) { return static_cast<u16>(~sum(bytes)); }

struct Ethernet
{
    Mac destination{}, source{};
    u16 type{};
    Bytes payload{};
};
inline Result<Ethernet> ethernet(Bytes frame)
{
    if (frame.size() < 14) return Status::invalid_argument("truncated Ethernet header");
    Ethernet view;
    for (usize i = 0; i < 6; ++i) { view.destination[i] = byte(frame, i); view.source[i] = byte(frame, i + 6); }
    view.type = be16(frame, 12);
    if (view.type < 0x0600) return Status::unsupported("IEEE 802.3 length frames are not in the current profile");
    if (view.type == 0x8100 || view.type == 0x88a8) return Status::unsupported("VLAN frames are not in the current profile");
    view.payload = frame.subspan(14);
    return view;
}

struct Arp
{
    u16 operation{};
    Mac sender_mac{}, target_mac{};
    Ipv4 sender_ip{}, target_ip{};
};
inline Result<Arp> arp(Bytes payload)
{
    if (payload.size() < 28) return Status::invalid_argument("truncated Ethernet IPv4 ARP packet");
    if (be16(payload, 0) != 1 || be16(payload, 2) != 0x0800 || byte(payload, 4) != 6 || byte(payload, 5) != 4)
        return Status::unsupported("ARP address formats are outside the Ethernet IPv4 profile");
    Arp view; view.operation = be16(payload, 6);
    if (view.operation != 1 && view.operation != 2) return Status::unsupported("ARP operation is not request or reply");
    for (usize i = 0; i < 6; ++i) { view.sender_mac[i] = byte(payload, 8 + i); view.target_mac[i] = byte(payload, 18 + i); }
    for (usize i = 0; i < 4; ++i) { view.sender_ip[i] = byte(payload, 14 + i); view.target_ip[i] = byte(payload, 24 + i); }
    // Sender address policy, conflict detection and neighbor expiry belong to
    // the receiving interface. Parsing an ARP claim never learns a neighbor.
    return view;
}

struct Ipv4Packet
{
    Ipv4 source{}, destination{};
    u8 traffic_class{}, ttl{}, protocol{};
    u16 identification{};
    bool dont_fragment{};
    Bytes payload{};
};
inline Result<Ipv4Packet> ipv4(Bytes packet)
{
    if (packet.size() < 20) return Status::invalid_argument("truncated IPv4 header");
    if ((byte(packet, 0) >> 4) != 4) return Status::unsupported("packet is not IPv4");
    const usize header = (byte(packet, 0) & 15u) * 4u;
    if (header < 20 || header > packet.size()) return Status::invalid_argument("invalid IPv4 header length");
    const usize length = be16(packet, 2);
    if (length < header || length > packet.size()) return Status::invalid_argument("invalid IPv4 total length");
    if (checksum(packet.first(header)) != 0) return Status::invalid_argument("invalid IPv4 header checksum");
    const u16 fragments = be16(packet, 6);
    if (fragments & 0x8000) return Status::invalid_argument("IPv4 reserved fragment bit is set");
    if (fragments & 0x3fff) return Status::unsupported("IPv4 fragment reassembly is not yet available");
    if (header != 20) return Status::unsupported("IPv4 options are not yet available");
    Ipv4Packet view;
    for (usize i = 0; i < 4; ++i) { view.source[i] = byte(packet, 12 + i); view.destination[i] = byte(packet, 16 + i); }
    view.traffic_class = byte(packet, 1); view.ttl = byte(packet, 8); view.protocol = byte(packet, 9);
    view.identification = be16(packet, 4); view.dont_fragment = fragments & 0x4000;
    view.payload = packet.subspan(header, length - header);
    return view;
}

inline u32 pseudo_sum(Ipv4 source, Ipv4 destination, u8 protocol, u16 length)
{
    u32 value = protocol + length;
    for (usize i = 0; i < 4; i += 2)
        value += (u16{source[i]} << 8) | source[i + 1];
    for (usize i = 0; i < 4; i += 2)
        value += (u16{destination[i]} << 8) | destination[i + 1];
    return (value & 0xffffu) + (value >> 16);
}
struct Udp
{
    u16 source_port{}, destination_port{};
    bool checksum_present{};
    Bytes payload{};
};
inline Result<Udp> udp(const Ipv4Packet &ip)
{
    if (ip.protocol != 17) return Status::unsupported("IPv4 payload is not UDP");
    if (ip.payload.size() < 8) return Status::invalid_argument("truncated UDP header");
    const usize length = be16(ip.payload, 4);
    if (length < 8 || length > ip.payload.size()) return Status::invalid_argument("invalid UDP length");
    const auto datagram = ip.payload.first(length);
    Udp view; view.source_port = be16(datagram, 0); view.destination_port = be16(datagram, 2);
    if (!view.destination_port) return Status::invalid_argument("UDP destination port zero is reserved");
    view.checksum_present = be16(datagram, 6) != 0;
    if (view.checksum_present && sum(datagram, pseudo_sum(ip.source, ip.destination, 17, static_cast<u16>(length))) != 0xffffu)
        return Status::invalid_argument("invalid IPv4 UDP pseudo-header checksum");
    // IPv4 permits an omitted UDP checksum. Extra IP payload does not become
    // datagram data; Ethernet padding was already trimmed by IPv4 total length.
    view.payload = datagram.subspan(8);
    return view;
}

// Borrowed views above remain valid only while the caller retains the packet.
// Payload and the emitted output bytes must be disjoint. An unused output tail
// may contain input; invalid calls leave every output byte unchanged.
inline Result<usize> build_udp(std::span<std::byte> output, Ipv4 source, Ipv4 destination,
                               u16 source_port, u16 destination_port, u16 identification,
                               u8 ttl, Bytes payload)
{
    // Linux connected UDP can transmit to zero; ingress policy is separate.
    if (!ttl) return Status::invalid_argument("UDP send TTL must be nonzero");
    if (payload.size() > 65507 || output.size() < 28 + payload.size()) return Status::overflow("IPv4 UDP packet exceeds output capacity");
    const uptr first = reinterpret_cast<uptr>(output.data()), input = reinterpret_cast<uptr>(payload.data());
    const usize length = 28 + payload.size();
    if (!payload.empty() && (input >= first ? input - first < length : first - input < payload.size()))
        return Status::invalid_argument("IPv4 UDP input aliases output");
    for (usize i = 0; i < 28; ++i) output[i] = std::byte{};
    output[0] = std::byte{0x45}; output[8] = std::byte{ttl}; output[9] = std::byte{17};
    be16(output, 2, static_cast<u16>(length)); be16(output, 4, identification); be16(output, 6, 0x4000);
    for (usize i = 0; i < 4; ++i) { output[12 + i] = std::byte{source[i]}; output[16 + i] = std::byte{destination[i]}; }
    be16(output, 10, checksum(Bytes{output.data(), 20}));
    be16(output, 20, source_port); be16(output, 22, destination_port); be16(output, 24, static_cast<u16>(length - 20));
    for (usize i = 0; i < payload.size(); ++i) output[28 + i] = payload[i];
    u16 transport_checksum = static_cast<u16>(~sum(Bytes{output.data() + 20, length - 20},
        pseudo_sum(source, destination, 17, static_cast<u16>(length - 20))));
    if (!transport_checksum) transport_checksum = 0xffff;
    be16(output, 26, transport_checksum);
    return length;
}
}
