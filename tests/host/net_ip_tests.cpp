#include <ok/net/icmp.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ok;
namespace wire = ok::net::wire;
using V4 = wire::Ipv4Family;
using V6 = wire::Ipv6Family;

namespace
{
usize checks{};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "NET_IP_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #condition); \
    std::abort(); } } while (false)

using Buffer = std::vector<std::byte>;
Buffer decode(const char *hex)
{
    auto digit = [](char value) -> u8 {
        if (value >= '0' && value <= '9') return static_cast<u8>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<u8>(value - 'a' + 10);
        CHECK(false); return 0;
    };
    Buffer out;
    for (usize i = 0; hex[i]; i += 2)
    {
        CHECK(hex[i + 1]);
        out.push_back(std::byte{static_cast<u8>((digit(hex[i]) << 4) | digit(hex[i + 1]))});
    }
    return out;
}
Buffer text(const char *value) { Buffer out; for (usize i = 0; value[i]; ++i) out.push_back(std::byte{static_cast<u8>(value[i])}); return out; }
bool same(wire::Bytes a, wire::Bytes b) { return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(), b.data(), a.size())); }
wire::Ipv6 address(std::initializer_list<u16> groups)
{
    wire::Ipv6 out{}; usize at = 0;
    for (u16 group : groups) { out[at] = static_cast<u8>(group >> 8); out[at + 1] = static_cast<u8>(group); at += 2; }
    return out;
}
const wire::Ipv6 doc1 = address({0x2001, 0x0db8, 0, 0, 0, 0, 0, 1});
const wire::Ipv6 doc2 = address({0x2001, 0x0db8, 0, 0, 0, 0, 0, 2});
const wire::Ipv6 ll1 = address({0xfe80, 0, 0, 0, 0, 0, 0, 1});
const wire::Ipv6 ll2 = address({0xfe80, 0, 0, 0, 0, 0, 0, 2});
const wire::Ipv6 host = address({0xfe80, 0, 0, 0, 0x0200, 0x00ff, 0xfe00, 1});

// Independently generated (Python struct packing and its own checksum).
const char *udp6 = "60000000001a114020010db800000000000000000000000120010db80000000000000000000000023039d431001a04084f62667573636174696f6e4f5300756470ff";
const char *tcp6 = "6000000000180640fe800000000000000000000000000001fe8000000000000000000000000000029c400050010203040000000060021000ea9f0000020405a0";
const char *echo6 = "60000000000d3a40fe80000000000000020000fffe000001fe80000000000000000000000000000280002a994242000770696e6736";
const char *reply6 = "60000000000d3a40fe800000000000000000000000000002fe80000000000000020000fffe000001810029994242000770696e6736";
const char *ns6 = "6000000000203afffe80000000000000020000fffe000001ff0200000000000000000001ff0000028700799700000000fe8000000000000000000000000000020101020000000001";
const char *na6 = "6000000000203afffe800000000000000000000000000002fe80000000000000020000fffe0000018800171a60000000fe8000000000000000000000000000020201020000000002";
const char *ra6 = "6000000000403afffe800000000000000000000000000002ff0200000000000000000000000000018600db7f4000070800000000000000000101525600000002030440c000015180000038400000000020010db800010000000000000000000005010000000005dc";
const char *hbh6 = "6000000000100001fe80000000000000020000fffe000001ff0200000000000000000000000000163a00050200000100c800392200000000";
const char *echo4 = "45000022123440004001a4a3c0000201c00002020800d2d81234000170696e673421";

void addresses()
{
    CHECK(wire::solicited_node(ll2) == address({0xff02, 0, 0, 0, 0, 1, 0xff00, 2}));
    CHECK((wire::multicast_mac(wire::solicited_node(ll2)) == wire::Mac{0x33, 0x33, 0xff, 0, 0, 2}));
    CHECK(wire::link_local_from_mac({0, 0, 0, 0, 0, 1}) == host);
    CHECK(wire::link_local_from_mac({2, 0, 0, 0, 0, 1}) == address({0xfe80, 0, 0, 0, 0, 0x00ff, 0xfe00, 1}));
    CHECK(wire::link_local(ll1) && !wire::link_local(doc1) && wire::multicast(wire::all_nodes));
    CHECK(wire::loopback(address({0, 0, 0, 0, 0, 0, 0, 1})) && wire::unspecified(wire::Ipv6{}));
    CHECK(wire::ipv4_mapped(address({0, 0, 0, 0, 0, 0xffff, 0xc000, 0x0201})) && !wire::ipv4_mapped(doc1));
    CHECK(wire::prefix_match(doc1, doc2, 64) && wire::prefix_match(doc1, doc2, 126) && !wire::prefix_match(doc1, doc2, 127));
    CHECK(wire::prefix_match(doc1, ll1, 0));
    CHECK(wire::prefix_match(wire::Ipv4{10, 0, 2, 15}, wire::Ipv4{10, 0, 2, 2}, 24));
    CHECK(!wire::prefix_match(wire::Ipv4{10, 0, 3, 15}, wire::Ipv4{10, 0, 2, 2}, 24));
    CHECK(wire::prefix_match(wire::Ipv4{10, 0, 3, 15}, wire::Ipv4{10, 0, 2, 2}, 23));
    CHECK(!wire::prefix_match(wire::Ipv4{10, 0, 4, 15}, wire::Ipv4{10, 0, 2, 2}, 23));
    CHECK(wire::multicast(wire::Ipv4{224, 0, 0, 1}) && !wire::multicast(wire::Ipv4{240, 0, 0, 1}));
    CHECK(wire::broadcast(wire::Ipv4{255, 255, 255, 255}) && wire::multicast(wire::broadcast_mac));
}

void ipv6_transport_golden()
{
    const auto golden = decode(udp6); const auto payload = text("ObfuscationOS");
    auto data = payload; data.push_back(std::byte{}); for (auto c : text("udp")) data.push_back(c); data.push_back(std::byte{0xff});
    auto ip = wire::ipv6(golden); CHECK(ip);
    CHECK(ip.value().source == doc1 && ip.value().destination == doc2 && ip.value().hop_limit == 64);
    CHECK(ip.value().protocol == 17 && ip.value().payload.data() == golden.data() + 40);
    auto udp = wire::udp<V6>(ip.value()); CHECK(udp);
    CHECK(udp.value().source_port == 12345 && udp.value().destination_port == 54321 && same(udp.value().payload, data));
    std::array<std::byte, 128> output{}; output.fill(std::byte{0x5a});
    auto built = wire::build_udp<V6>(output, doc1, doc2, 12345, 54321, 0, 64, data);
    CHECK(built && same({output.data(), built.value()}, golden));
    for (usize i = golden.size(); i < output.size(); ++i) CHECK(output[i] == std::byte{0x5a});
    // Every single-bit error in the upper layer fails its checksum or shape.
    for (usize at = 40; at < golden.size(); ++at)
        for (u8 bit = 0; bit < 8; ++bit)
        {
            auto changed = golden; changed[at] ^= std::byte{static_cast<u8>(1u << bit)};
            auto parsed = wire::ipv6(changed); CHECK(parsed);
            CHECK(!wire::udp<V6>(parsed.value()));
        }
    auto zero = golden; zero[46] = zero[47] = std::byte{};
    auto zero_ip = wire::ipv6(zero); CHECK(zero_ip && !wire::udp<V6>(zero_ip.value()));
    for (usize length = 0; length < golden.size(); ++length)
    {
        auto prefix = wire::ipv6({golden.data(), length});
        CHECK(!prefix || !wire::udp<V6>(prefix.value()));
    }

    const auto segment = decode(tcp6);
    auto tip = wire::ipv6(segment); CHECK(tip);
    auto tcp = wire::tcp<V6>(tip.value()); CHECK(tcp);
    const auto &f = tcp.value().fields;
    CHECK(f.source_port == 40000 && f.destination_port == 80 && f.sequence == 0x01020304 && f.flags == wire::tcp_syn);
    CHECK(f.window == 4096 && tcp.value().options.size() == 4 && tcp.value().payload.empty());
    std::array<std::byte, 4> mss{std::byte{2}, std::byte{4}, std::byte{5}, std::byte{0xa0}};
    auto rebuilt = wire::build_tcp<V6>(output, ll1, ll2, 0, 64, f, mss, {});
    CHECK(rebuilt && same({output.data(), rebuilt.value()}, segment));
    for (usize at = 40; at < segment.size(); ++at)
        for (u8 bit = 0; bit < 8; ++bit)
        {
            auto changed = segment; changed[at] ^= std::byte{static_cast<u8>(1u << bit)};
            auto parsed = wire::ipv6(changed); CHECK(parsed && !wire::tcp<V6>(parsed.value()));
        }
    // The IPv4 UDP/TCP parsers do not accept an IPv6 view and vice versa.
    CHECK(!wire::ipv4(golden) && !wire::ipv6(decode(echo4)));
}

void ipv4_generic_matches_legacy()
{
    const wire::Ipv4 source{192, 0, 2, 1}, destination{198, 51, 100, 2};
    std::array<std::byte, 1600> legacy{}, generic{};
    Buffer payload;
    u32 seed = 0x1234567u;
    for (usize size = 0; size <= 1400; size += 37)
    {
        payload.resize(size);
        for (auto &value : payload) { seed = seed * 1103515245u + 12345u; value = std::byte{static_cast<u8>(seed >> 16)}; }
        const u16 sport = static_cast<u16>(seed), dport = static_cast<u16>((seed >> 8) | 1u);
        auto a = wire::build_udp(legacy, source, destination, sport, dport, static_cast<u16>(size), 64, payload);
        auto b = wire::build_udp<V4>(generic, source, destination, sport, dport, static_cast<u16>(size), 64, payload);
        CHECK(a && b && same({legacy.data(), a.value()}, {generic.data(), b.value()}));
        auto ip = wire::ipv4({generic.data(), b.value()}); CHECK(ip);
        auto u1 = wire::udp(ip.value()); auto u2 = wire::udp<V4>(ip.value());
        CHECK(u1 && u2 && u1.value().source_port == u2.value().source_port && same(u1.value().payload, u2.value().payload));
        wire::TcpFields fields{sport, dport, seed, ~seed, wire::tcp_ack | wire::tcp_psh, 4096, 0};
        auto c = wire::build_tcp(legacy, source, destination, 7, 64, fields, {}, payload);
        auto d = wire::build_tcp<V4>(generic, source, destination, 7, 64, fields, {}, payload);
        CHECK(c && d && same({legacy.data(), c.value()}, {generic.data(), d.value()}));
        auto tip = wire::ipv4({generic.data(), d.value()}); CHECK(tip);
        auto t1 = wire::tcp(tip.value()); auto t2 = wire::tcp<V4>(tip.value());
        CHECK(t1 && t2 && t1.value().fields.sequence == t2.value().fields.sequence && same(t1.value().payload, t2.value().payload));
    }
    // A zero IPv4 UDP checksum still means "absent".
    const auto golden = decode("4500002e1234400040113c54c0000201c63364023039d431001a7345"
                               "4f62667573636174696f6e4f5300756470ff");
    auto zero = golden; zero[26] = zero[27] = std::byte{};
    auto ip = wire::ipv4(zero); CHECK(ip);
    auto udp = wire::udp<V4>(ip.value()); CHECK(udp && !udp.value().checksum_present);
}

void icmp_golden()
{
    const auto request = decode(echo6), reply = decode(reply6);
    auto ip = wire::ipv6(request); CHECK(ip);
    auto message = wire::icmp<V6>(ip.value()); CHECK(message);
    CHECK(message.value().type == wire::icmpv6::echo_request && message.value().code == 0);
    CHECK(message.value().identifier() == 0x4242 && message.value().sequence() == 7 && same(message.value().body, text("ping6")));
    std::array<std::byte, 128> output{};
    auto built = wire::build_icmp<V6>(output, ip.value().destination, ip.value().source, 64, 0,
                                      wire::icmpv6::echo_reply, 0, message.value().rest, message.value().body);
    CHECK(built && same({output.data(), built.value()}, reply));
    for (usize at = 40; at < request.size(); ++at)
    {
        auto changed = request; changed[at] ^= std::byte{1};
        auto parsed = wire::ipv6(changed); CHECK(parsed && !wire::icmp<V6>(parsed.value()));
    }
    // The ICMPv6 checksum covers the pseudo header; an IPv4-style sum does not.
    auto swapped = request; for (usize i = 0; i < 16; ++i) std::swap(swapped[8 + i], swapped[24 + i]);
    auto swapped_ip = wire::ipv6(swapped); CHECK(swapped_ip && wire::icmp<V6>(swapped_ip.value()));
    swapped[39] ^= std::byte{1};
    swapped_ip = wire::ipv6(swapped); CHECK(swapped_ip && !wire::icmp<V6>(swapped_ip.value()));

    const auto v4 = decode(echo4);
    auto ip4 = wire::ipv4(v4); CHECK(ip4);
    auto m4 = wire::icmp<V4>(ip4.value()); CHECK(m4);
    CHECK(m4.value().type == wire::icmpv4::echo_request && m4.value().identifier() == 0x1234 && m4.value().sequence() == 1);
    auto rebuilt = wire::build_icmp<V4>(output, ip4.value().source, ip4.value().destination, 64, 0x1234,
                                        wire::icmpv4::echo_request, 0, m4.value().rest, m4.value().body);
    CHECK(rebuilt && same({output.data(), rebuilt.value()}, v4));
    for (usize at = 20; at < v4.size(); ++at)
    {
        auto changed = v4; changed[at] ^= std::byte{0x80};
        auto parsed = wire::ipv4(changed); CHECK(parsed && !wire::icmp<V4>(parsed.value()));
    }
    CHECK(wire::icmp_error<V6>(wire::icmpv6::unreachable) && !wire::icmp_error<V6>(wire::icmpv6::echo_request));
    CHECK(wire::icmp_error<V4>(wire::icmpv4::unreachable) && !wire::icmp_error<V4>(wire::icmpv4::echo_reply));

    // Errors quote what fits in 576 (IPv4) or 1280 (IPv6) bytes.
    Buffer offending(2000, std::byte{0x77});
    std::array<std::byte, 1600> large{};
    auto e4 = wire::build_icmp_error<V4>(large, {192, 0, 2, 2}, {192, 0, 2, 1}, 1, wire::icmpv4::unreachable,
                                         wire::icmpv4::port_unreachable, 0, offending);
    CHECK(e4 && e4.value() == 576);
    auto e6 = wire::build_icmp_error<V6>(large, doc2, doc1, 0, wire::icmpv6::unreachable, wire::icmpv6::port_unreachable, 0, offending);
    CHECK(e6 && e6.value() == 1280);
    auto e6ip = wire::ipv6({large.data(), e6.value()}); CHECK(e6ip);
    auto e6m = wire::icmp<V6>(e6ip.value()); CHECK(e6m && e6m.value().body.size() == 1280 - 48);
    auto short4 = wire::build_icmp_error<V4>(large, {192, 0, 2, 2}, {192, 0, 2, 1}, 1, wire::icmpv4::unreachable,
                                             wire::icmpv4::port_unreachable, 0, {offending.data(), 28});
    CHECK(short4 && short4.value() == 20 + 8 + 28);
    CHECK(!wire::build_icmp_error<V4>(large, {192, 0, 2, 2}, {192, 0, 2, 1}, 1, wire::icmpv4::echo_reply, 0, 0, offending));
}

void neighbour_discovery()
{
    const auto solicitation = decode(ns6);
    auto ip = wire::ipv6(solicitation); CHECK(ip);
    auto message = wire::icmp<V6>(ip.value()); CHECK(message);
    auto nd = wire::neighbor_discovery(ip.value(), message.value()); CHECK(nd);
    CHECK(nd.value().kind == wire::NdKind::neighbor_solicitation && nd.value().target == ll2);
    CHECK(nd.value().source_link && (*nd.value().source_link == wire::Mac{2, 0, 0, 0, 0, 1}) && !nd.value().target_link);
    std::array<std::byte, 128> output{};
    auto built = wire::build_neighbor_solicitation(output, host, wire::solicited_node(ll2), ll2, wire::Mac{2, 0, 0, 0, 0, 1});
    CHECK(built && same({output.data(), built.value()}, solicitation));
    // Hop limit 255 is the on-link proof; it is outside the checksum.
    auto routed = solicitation; routed[7] = std::byte{254};
    auto routed_ip = wire::ipv6(routed); auto routed_message = wire::icmp<V6>(routed_ip.value());
    CHECK(routed_message && !wire::neighbor_discovery(routed_ip.value(), routed_message.value()));

    const auto advertisement = decode(na6);
    ip = wire::ipv6(advertisement); message = wire::icmp<V6>(ip.value()); CHECK(message);
    nd = wire::neighbor_discovery(ip.value(), message.value()); CHECK(nd);
    CHECK(nd.value().kind == wire::NdKind::neighbor_advertisement && nd.value().target == ll2);
    CHECK(!nd.value().router && nd.value().solicited && nd.value().override_flag);
    CHECK(nd.value().target_link && (*nd.value().target_link == wire::Mac{2, 0, 0, 0, 0, 2}));
    built = wire::build_neighbor_advertisement(output, ll2, host, ll2, false, true, true, wire::Mac{2, 0, 0, 0, 0, 2});
    CHECK(built && same({output.data(), built.value()}, advertisement));
    CHECK(!wire::build_neighbor_advertisement(output, ll2, wire::all_nodes, ll2, false, true, true, {}));
    CHECK(!wire::build_neighbor_solicitation(output, wire::Ipv6{}, wire::solicited_node(ll2), ll2, wire::Mac{}));

    // Duplicate-address probes: unspecified source, solicited-node destination, no SLLA.
    built = wire::build_neighbor_solicitation(output, wire::Ipv6{}, wire::solicited_node(ll2), ll2, {});
    CHECK(built);
    ip = wire::ipv6({output.data(), built.value()}); message = wire::icmp<V6>(ip.value()); CHECK(message);
    CHECK(wire::neighbor_discovery(ip.value(), message.value()));
    built = wire::build_neighbor_solicitation(output, wire::Ipv6{}, wire::all_nodes, ll2, {});
    ip = wire::ipv6({output.data(), built.value()}); message = wire::icmp<V6>(ip.value()); CHECK(message);
    CHECK(!wire::neighbor_discovery(ip.value(), message.value()));

    const auto router = decode(ra6);
    ip = wire::ipv6(router); message = wire::icmp<V6>(ip.value()); CHECK(message);
    nd = wire::neighbor_discovery(ip.value(), message.value()); CHECK(nd);
    const auto &ra = nd.value();
    CHECK(ra.kind == wire::NdKind::router_advertisement && ra.hop_limit == 64 && ra.router_lifetime == 1800);
    CHECK(ra.source_link && (*ra.source_link == wire::Mac{0x52, 0x56, 0, 0, 0, 2}) && ra.mtu && *ra.mtu == 1500);
    CHECK(ra.prefix_count == 1 && ra.prefixes[0].length == 64 && ra.prefixes[0].on_link && ra.prefixes[0].autonomous);
    CHECK(ra.prefixes[0].valid_lifetime == 86400 && ra.prefixes[0].preferred_lifetime == 14400);
    CHECK(ra.prefixes[0].prefix == address({0x2001, 0x0db8, 1, 0, 0, 0, 0, 0}));

    built = wire::build_router_solicitation(output, host, wire::Mac{0, 0, 0, 0, 0, 1});
    ip = wire::ipv6({output.data(), built.value()}); message = wire::icmp<V6>(ip.value()); CHECK(message);
    nd = wire::neighbor_discovery(ip.value(), message.value());
    CHECK(nd && nd.value().kind == wire::NdKind::router_solicitation && ip.value().destination == wire::all_routers);

    // A zero-length option would loop forever; it invalidates the message.
    auto zero_option = solicitation; zero_option[65] = std::byte{0};
    ip = wire::ipv6(zero_option);
    Buffer fixed(zero_option.begin(), zero_option.end());
    fixed[42] = fixed[43] = std::byte{};
    const u32 initial = V6::icmp_initial(ip.value().source, ip.value().destination, ip.value().payload.size());
    wire::be16(fixed, 42, static_cast<u16>(~wire::sum({fixed.data() + 40, fixed.size() - 40}, initial)));
    ip = wire::ipv6(fixed); message = wire::icmp<V6>(ip.value()); CHECK(message);
    CHECK(!wire::neighbor_discovery(ip.value(), message.value()));
}

void extension_headers()
{
    const auto packet = decode(hbh6);
    auto ip = wire::ipv6(packet); CHECK(ip);
    CHECK(ip.value().protocol == 58 && ip.value().payload.data() == packet.data() + 48 && ip.value().payload.size() == 8);
    auto message = wire::icmp<V6>(ip.value()); CHECK(message && message.value().type == 200);
    auto discard = packet; discard[42] = std::byte{0x85};
    CHECK(!wire::ipv6(discard));
    auto fragment = packet; fragment[6] = std::byte{44};
    CHECK(wire::ipv6(fragment).status().code() == StatusCode::unsupported);
    // Hop-by-hop must directly follow the fixed header.
    Buffer twice(packet.begin(), packet.begin() + 40);
    twice[6] = std::byte{60}; wire::be16(twice, 4, 24);
    const auto options = decode("0000010400000000");
    for (auto value : options) twice.push_back(value);
    for (usize i = 40; i < packet.size(); ++i) twice.push_back(packet[i]);
    CHECK(!wire::ipv6(twice));
    twice[40] = std::byte{58};
    ip = wire::ipv6(twice); CHECK(ip && ip.value().protocol == 58);
    // A routing header still to be processed stays unsupported.
    auto routing = twice; routing[6] = std::byte{43}; routing[42] = std::byte{0}; routing[43] = std::byte{1};
    CHECK(wire::ipv6(routing).status().code() == StatusCode::unsupported);
    routing[43] = std::byte{0};
    CHECK(wire::ipv6(routing));
    auto multicast_source = packet; multicast_source[8] = std::byte{0xff};
    CHECK(!wire::ipv6(multicast_source));
    auto too_long = packet; wire::be16(too_long, 4, 17);
    CHECK(!wire::ipv6(too_long));
    auto version = packet; version[0] = std::byte{0x40};
    CHECK(wire::ipv6(version).status().code() == StatusCode::unsupported);
    CHECK(wire::ip_version(packet) == 6 && wire::ip_version(decode(echo4)) == 4 && wire::ip_version({}) == 0);
}

void link_layer_builders()
{
    std::array<std::byte, 64> frame{}; frame.fill(std::byte{0xee});
    auto header = wire::build_ethernet(frame, wire::broadcast_mac, {2, 0, 0, 0, 0, 1}, wire::ethertype_arp, 28);
    CHECK(header && header.value() == 42);
    auto arp = wire::build_arp(std::span{frame}.subspan(14), 1, {2, 0, 0, 0, 0, 1}, {10, 0, 2, 15}, {}, {10, 0, 2, 2});
    CHECK(arp && arp.value() == 28);
    auto eth = wire::ethernet({frame.data(), 42}); CHECK(eth && eth.value().type == wire::ethertype_arp);
    auto claim = wire::arp(eth.value().payload); CHECK(claim);
    CHECK(claim.value().operation == 1 && (claim.value().sender_ip == wire::Ipv4{10, 0, 2, 15}) &&
          (claim.value().target_ip == wire::Ipv4{10, 0, 2, 2}) && (claim.value().sender_mac == wire::Mac{2, 0, 0, 0, 0, 1}));
    CHECK(!wire::build_ethernet(frame, {}, {}, 0x05dc, 0));
    CHECK(!wire::build_ethernet(frame, {}, {}, wire::ethertype_ipv4, 51));
    CHECK(!wire::build_arp(std::span{frame}.first(27), 1, {}, {}, {}, {}));
    CHECK(!wire::build_arp(frame, 3, {}, {}, {}, {}));
}

void invalid_builders_leave_output()
{
    std::array<std::byte, 128> output{}; output.fill(std::byte{0x3c});
    const auto before = output;
    Buffer payload(16, std::byte{1});
    CHECK(!wire::build_udp<V6>(output, doc1, doc2, 1, 2, 0, 0, payload));
    CHECK(!wire::build_udp<V6>(std::span{output}.first(63), doc1, doc2, 1, 2, 0, 64, payload));
    CHECK(!wire::build_udp<V6>(output, doc1, doc2, 1, 2, 0, 64, {output.data() + 50, 4}));
    CHECK(!wire::build_tcp<V6>(output, doc1, doc2, 0, 64, {}, {output.data() + 64, 4}, {}));
    CHECK(!wire::build_icmp<V6>(output, doc1, doc2, 0, 0, 128, 0, 0, payload));
    CHECK(!wire::build_icmp<V4>(std::span{output}.first(43), {1, 1, 1, 1}, {2, 2, 2, 2}, 64, 0, 8, 0, 0, payload));
    CHECK(output == before);
    // Unused output tails may hold input.
    CHECK(wire::build_udp<V6>(output, doc1, doc2, 1, 2, 0, 64, {output.data() + 100, 4}));
}
}

int main()
{
    addresses();
    ipv6_transport_golden();
    ipv4_generic_matches_legacy();
    icmp_golden();
    neighbour_discovery();
    extension_headers();
    link_layer_builders();
    invalid_builders_leave_output();
    std::printf("PASS net ip scope=model checks=%zu\n", checks);
}
