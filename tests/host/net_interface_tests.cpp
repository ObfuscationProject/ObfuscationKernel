#include <ok/net/interface.hpp>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ok;
namespace wire = ok::net::wire;
using V4 = wire::Ipv4Family;
using V6 = wire::Ipv6Family;

namespace
{
usize checks{};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "NET_INTERFACE_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #condition); \
    std::abort(); } } while (false)

using Frame = std::vector<std::byte>;
const wire::Mac self_mac{0x52, 0x54, 0, 0x12, 0x34, 0x56}, router_mac{0x52, 0x55, 0x0a, 0, 2, 2}, host_mac{2, 0, 0, 0, 0, 3};
const wire::Ipv4 self4{10, 0, 2, 15}, router4{10, 0, 2, 2}, host4{10, 0, 2, 3}, remote4{93, 184, 216, 34};
const wire::Ipv6 router6 = wire::link_local_from_mac(router_mac);
const wire::Ipv6 self6 = wire::link_local_from_mac(self_mac);
const wire::Ipv6 prefix{0x20, 0x01, 0x0d, 0xb8, 0, 1};
const wire::Ipv6 global6 = wire::with_interface_id(prefix, self_mac);
const wire::Ipv6 remote6{0x26, 0x06, 0x47, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

net::InterfaceConfig config(bool ipv6 = true)
{ return {self_mac, 1500, net::Ipv4Configuration{self4, 24, router4, {}}, ipv6}; }

template <class Build> Frame frame(const wire::Mac &destination, const wire::Mac &source, u16 type, Build &&build)
{
    Frame out(1514);
    auto length = build(std::span{out}.subspan(14)); CHECK(length);
    auto header = wire::build_ethernet(out, destination, source, type, length.value()); CHECK(header);
    out.resize(header.value() < 60 ? 60 : header.value());
    return out;
}
Frame arp(u16 op, const wire::Mac &eth_destination, const wire::Mac &mac, const wire::Ipv4 &ip, const wire::Mac &target_mac,
          const wire::Ipv4 &target)
{ return frame(eth_destination, mac, wire::ethertype_arp, [&](std::span<std::byte> o) { return wire::build_arp(o, op, mac, ip, target_mac, target); }); }
const std::array<std::byte, 3> payload{std::byte{1}, std::byte{2}, std::byte{3}};
Frame udp4(const wire::Mac &destination_mac, const wire::Ipv4 &destination, u16 port)
{
    return frame(destination_mac, router_mac, wire::ethertype_ipv4,
                 [&](std::span<std::byte> o) { return wire::build_udp<V4>(o, remote4, destination, 4000, port, 9, 64, payload); });
}
Frame icmp4(const wire::Ipv4 &source, const wire::Ipv4 &destination, u8 type, u16 id, u16 seq)
{
    return frame(self_mac, router_mac, wire::ethertype_ipv4, [&](std::span<std::byte> o) {
        return wire::build_icmp<V4>(o, source, destination, 64, 1, type, 0, (u32{id} << 16) | seq, payload); });
}
template <class Build> Frame ip6(const wire::Mac &destination, const wire::Mac &source, Build &&build)
{ return frame(destination, source, wire::ethertype_ipv6, build); }
Frame router_advertisement(u16 lifetime, u32 valid, u32 preferred)
{
    // RA with SLLA, a /64 on-link autonomous prefix and an MTU of 1480.
    // The header word holds the hop limit, flags and router lifetime; the body
    // starts with zero reachable and retransmit times.
    std::array<std::byte, 8 + 8 + 32 + 8> body{};
    body[8] = std::byte{1}; body[9] = std::byte{1};
    for (usize i = 0; i < 6; ++i) body[10 + i] = std::byte{router_mac[i]};
    body[16] = std::byte{3}; body[17] = std::byte{4}; body[18] = std::byte{64}; body[19] = std::byte{0xc0};
    wire::tcp_detail::be32(body, 20, valid); wire::tcp_detail::be32(body, 24, preferred);
    for (usize i = 0; i < 16; ++i) body[32 + i] = std::byte{prefix[i]};
    body[48] = std::byte{5}; body[49] = std::byte{1}; wire::tcp_detail::be32(body, 52, 1480);
    return ip6(wire::multicast_mac(wire::all_nodes), router_mac, [&](std::span<std::byte> o) {
        return wire::build_icmp<V6>(o, router6, wire::all_nodes, 255, 0, wire::icmpv6::router_advertisement, 0,
                                    (u32{64} << 24) | lifetime, body); });
}

Frame take(net::EthernetInterface &nic)
{
    auto next = nic.control(); CHECK(next);
    Frame out(next->begin(), next->end()); nic.pop_control(); return out;
}
wire::Ethernet eth(const Frame &f) { auto parsed = wire::ethernet(f); CHECK(parsed); return parsed.value(); }
// Parsed views own their frame; a moved vector keeps its buffer.
struct Icmp6 { Frame bytes; wire::Ethernet link; wire::Ipv6Packet ip; wire::Icmp message; };
Icmp6 icmp6(Frame f)
{
    Icmp6 out{}; out.bytes = std::move(f);
    out.link = eth(out.bytes); CHECK(out.link.type == wire::ethertype_ipv6);
    auto ip = wire::ipv6(out.link.payload); CHECK(ip); out.ip = ip.value();
    auto m = wire::icmp<V6>(out.ip); CHECK(m); out.message = m.value();
    return out;
}
struct Icmp4 { Frame bytes; wire::Ethernet link; wire::Ipv4Packet ip; wire::Icmp message; };
Icmp4 icmp4(Frame f)
{
    Icmp4 out{}; out.bytes = std::move(f);
    out.link = eth(out.bytes); CHECK(out.link.type == wire::ethertype_ipv4);
    auto ip = wire::ipv4(out.link.payload); CHECK(ip); out.ip = ip.value();
    auto m = wire::icmp<V4>(out.ip); CHECK(m); out.message = m.value();
    return out;
}
wire::NdMessage nd(const Icmp6 &p) { auto parsed = wire::neighbor_discovery(p.ip, p.message); CHECK(parsed); return parsed.value(); }

// A configured interface past link-local DAD and its first router solicitation.
void bring_up(net::EthernetInterface &nic)
{
    CHECK(nic.configure(config()));
    CHECK(nic.tick(0)); take(nic);
    CHECK(nic.tick(1000)); take(nic);
    CHECK(!nic.control());
}

void configuration_and_dad()
{
    net::EthernetInterface nic;
    CHECK(!nic.configure({wire::broadcast_mac}));
    CHECK(!nic.configure({self_mac, 1500, net::Ipv4Configuration{self4, 24, wire::Ipv4{10, 0, 3, 2}, {}}}));
    CHECK(!nic.configure({self_mac, 1500, net::Ipv4Configuration{{127, 0, 0, 1}, 8, {}, {}}}));
    CHECK(nic.configure(config()) && !nic.configure(config()));
    CHECK(nic.routes4().size() == 2 && nic.routes6().size() == 1);
    CHECK(nic.routable(remote4) && nic.routable(host4) && !nic.routable(self4) && !nic.routable(wire::Ipv4{224, 0, 0, 1}));
    // Tentative: unusable as a source, probed with one solicitation from ::.
    CHECK(!nic.local(self6) && !nic.source(router6));
    CHECK(nic.tick(0));
    auto probe = icmp6(take(nic)); auto message = nd(probe);
    CHECK(wire::unspecified(probe.ip.source) && probe.ip.destination == wire::solicited_node(self6));
    CHECK(probe.link.destination == wire::multicast_mac(wire::solicited_node(self6)) && probe.link.source == self_mac);
    CHECK(message.kind == wire::NdKind::neighbor_solicitation && message.target == self6 && !message.source_link);
    CHECK(!nic.control() && nic.next_deadline() == 1000u);
    CHECK(nic.tick(999) && !nic.control());
    // DAD completes; the first router solicitation follows at once, then two more 4 s apart.
    CHECK(nic.tick(1000) && nic.local(self6) && nic.source(router6) && nic.source(router6).value() == self6);
    auto rs = icmp6(take(nic));
    CHECK(rs.ip.source == self6 && rs.ip.destination == wire::all_routers && nd(rs).kind == wire::NdKind::router_solicitation);
    CHECK(nd(rs).source_link && *nd(rs).source_link == self_mac);
    CHECK(nic.tick(4999) && !nic.control() && nic.tick(5000) && nic.control()); take(nic);
    CHECK(nic.tick(9000)); take(nic);
    CHECK(nic.tick(13000) && !nic.control());
}

void duplicate_address()
{
    net::EthernetInterface nic;
    CHECK(nic.configure(config()) && nic.tick(0)); take(nic);
    // Another node defends our tentative link-local address.
    auto na = ip6(wire::multicast_mac(wire::all_nodes), host_mac, [&](std::span<std::byte> o) {
        return wire::build_neighbor_advertisement(o, self6, wire::all_nodes, self6, false, false, true, host_mac); });
    CHECK(nic.receive(na, 10).family == net::Delivery::control);
    CHECK(nic.stats().dad_conflicts == 1);
    bool duplicated = false;
    nic.each_ipv6([&](const net::Ipv6AddressSnapshot &a) { duplicated = a.address == self6 && a.state == net::Ipv6AddressState::duplicated; });
    CHECK(duplicated && nic.tick(2000) && !nic.local(self6) && !nic.source(router6));
    // A simultaneous probe for the same tentative address is also a conflict.
    net::EthernetInterface other;
    CHECK(other.configure(config()) && other.tick(0)); take(other);
    auto ns = ip6(wire::multicast_mac(wire::solicited_node(self6)), host_mac, [&](std::span<std::byte> o) {
        return wire::build_neighbor_solicitation(o, {}, wire::solicited_node(self6), self6, {}); });
    other.receive(ns, 10);
    CHECK(other.stats().dad_conflicts == 1 && other.tick(2000) && !other.local(self6));
}

void arp_and_routed_output()
{
    net::EthernetInterface nic; bring_up(nic);
    // The gateway asks for us: answered, and learned as STALE.
    CHECK(nic.receive(arp(1, wire::broadcast_mac, router_mac, router4, {}, self4), 0).family == net::Delivery::control);
    auto reply = take(nic); auto e = eth(reply);
    CHECK(e.destination == router_mac && e.type == wire::ethertype_arp && reply.size() == 60);
    auto claim = wire::arp(e.payload);
    CHECK(claim && claim.value().operation == 2 && claim.value().sender_mac == self_mac && claim.value().sender_ip == self4);
    CHECK(claim.value().target_mac == router_mac && claim.value().target_ip == router4);
    CHECK(nic.arp().find(router4) && nic.arp().find(router4)->state == net::NeighbourState::stale);
    // Requests for other addresses are not answered and do not create entries.
    nic.receive(arp(1, wire::broadcast_mac, host_mac, host4, {}, {10, 0, 2, 99}), 0);
    CHECK(!nic.control() && !nic.arp().find(host4));

    // Off-link goes to the gateway, already resolved.
    std::array<std::byte, 1514> out{};
    auto build = [&](const wire::Ipv4 &destination) {
        auto built = wire::build_udp<V4>(std::span{out}.subspan(14), self4, destination, 1000, 53, 1, 64, payload);
        CHECK(built); return built.value(); };
    auto sent = nic.output(out, build(remote4));
    CHECK(sent && sent.value() == 60 && eth(Frame(out.begin(), out.begin() + 60)).destination == router_mac);
    // On-link and unknown: held, then resolved by a broadcast ARP request.
    sent = nic.output(out, build(host4)); CHECK(sent && sent.value() == 0 && nic.stats().held == 1);
    CHECK(nic.tick(1500));
    auto request = take(nic); auto re = eth(request);
    auto asked = wire::arp(re.payload);
    CHECK(re.destination == wire::broadcast_mac && asked && asked.value().operation == 1 && asked.value().target_ip == host4);
    usize flushed = nic.flush([](wire::Bytes) { return true; });
    CHECK(flushed == 0);
    nic.receive(arp(2, self_mac, host_mac, host4, self_mac, self4), 1600);
    CHECK(nic.arp().find(host4)->state == net::NeighbourState::reachable);
    Frame released;
    flushed = nic.flush([&](wire::Bytes f) { released.assign(f.begin(), f.end()); return true; });
    CHECK(flushed == 1 && eth(released).destination == host_mac && eth(released).source == self_mac);
    auto ip = wire::ipv4(eth(released).payload); CHECK(ip && ip.value().destination == host4);
    // Broadcast and multicast need no resolution.
    sent = nic.output(out, build({10, 0, 2, 255})); CHECK(sent && eth(Frame(out.begin(), out.begin() + 60)).destination == wire::broadcast_mac);
    sent = nic.output(out, build({224, 0, 0, 251}));
    CHECK(sent && (eth(Frame(out.begin(), out.begin() + 60)).destination == wire::Mac{1, 0, 0x5e, 0, 0, 251}));
    // No route after the default route is gone is reported, not held.
    net::EthernetInterface isolated;
    CHECK(isolated.configure({self_mac, 1500, net::Ipv4Configuration{self4, 24, {}, {}}, false}));
    CHECK(!isolated.routable(remote4) && isolated.routable(host4));
    sent = isolated.output(out, build(remote4)); CHECK(!sent && isolated.stats().no_route == 1);
    // Unanswered resolution drops the held packet after three requests.
    sent = isolated.output(out, build(host4)); CHECK(sent && !sent.value());
    for (u64 now = 0; now <= 4000; now += 1000) CHECK(isolated.tick(now));
    CHECK(!isolated.arp().find(host4) && isolated.arp().stats().failed == 1);
}

void ipv4_delivery_and_errors()
{
    net::EthernetInterface nic; bring_up(nic);
    // Received views borrow the frame, which must outlive them.
    const auto datagram = udp4(self_mac, self4, 7);
    auto received = nic.receive(datagram, 0);
    CHECK(received.family == net::Delivery::ipv4 && received.protocol == wire::protocol_udp && !received.multicast);
    auto ip = wire::ipv4(received.packet); CHECK(ip && wire::udp<V4>(ip.value()));
    // Not for us: another host's address, another MAC.
    CHECK(nic.receive(udp4(self_mac, host4, 7), 0).family == net::Delivery::none);
    CHECK(nic.receive(udp4(host_mac, self4, 7), 0).family == net::Delivery::none && nic.stats().filtered == 2);
    // No receiver: port unreachable quoting the datagram, routed back via the gateway.
    CHECK(nic.receive(arp(1, wire::broadcast_mac, router_mac, router4, {}, self4), 0).family == net::Delivery::control); take(nic);
    nic.unreachable(received);
    auto error = icmp4(take(nic));
    CHECK(error.message.type == wire::icmpv4::unreachable && error.message.code == wire::icmpv4::port_unreachable);
    CHECK(error.ip.source == self4 && error.ip.destination == remote4 && error.link.destination == router_mac);
    CHECK(error.message.body.size() == received.packet.size());
    // Broadcast datagrams never elicit errors.
    const auto broadcast_frame = udp4(wire::broadcast_mac, {10, 0, 2, 255}, 7);
    auto broadcast = nic.receive(broadcast_frame, 0);
    CHECK(broadcast.family == net::Delivery::ipv4 && broadcast.multicast);
    nic.unreachable(broadcast); CHECK(!nic.control());
    // Rate limit: a burst of ten, then one per 100 ms.
    // bring_up left the clock at 1000 ms.
    for (int i = 0; i < 12; ++i) { nic.unreachable(nic.receive(udp4(self_mac, self4, 7), 1010)); while (nic.control()) nic.pop_control(); }
    CHECK(nic.stats().icmp_errors == 10 && nic.stats().icmp_rate_limited == 3);
    nic.unreachable(nic.receive(udp4(self_mac, self4, 7), 1210)); CHECK(nic.stats().icmp_errors == 11);
    while (nic.control()) nic.pop_control();

    // Echo: requests are answered with the same identifier, sequence and data.
    nic.receive(icmp4(router4, self4, wire::icmpv4::echo_request, 0x77, 3), 300);
    auto echo = icmp4(take(nic));
    CHECK(echo.message.type == wire::icmpv4::echo_reply && echo.message.identifier() == 0x77 && echo.message.sequence() == 3);
    CHECK(echo.ip.source == self4 && echo.ip.destination == router4 && echo.message.body.size() == payload.size());
    // Diagnostic ping and its recorded reply.
    CHECK(nic.ping<V4>(router4, 0x99, 1, payload));
    auto ping = icmp4(take(nic));
    CHECK(ping.message.type == wire::icmpv4::echo_request && ping.ip.destination == router4 && ping.link.destination == router_mac);
    nic.receive(icmp4(router4, self4, wire::icmpv4::echo_reply, 0x99, 1), 320);
    auto last = nic.last_echo_reply();
    CHECK(last && last->version == 4 && last->identifier == 0x99 && last->sequence == 1 && last->at == nic.now() && last->size == 3);
    // Spoofed own source and broadcast sources are dropped.
    CHECK(nic.receive(icmp4(self4, self4, wire::icmpv4::echo_request, 1, 1), 330).family == net::Delivery::none && !nic.control());
}

void ipv6_router_and_slaac()
{
    net::EthernetInterface nic; bring_up(nic);
    CHECK(!nic.routable(remote6));
    CHECK(nic.receive(router_advertisement(1800, 86400, 14400), 2000).family == net::Delivery::control);
    CHECK(nic.stats().router_advertisements == 1 && nic.ipv6_mtu() == 1480);
    CHECK(nic.neighbours().find(router6) && nic.neighbours().find(router6)->state == net::NeighbourState::stale);
    CHECK(nic.routable(remote6) && nic.routes6().size() == 3);
    // The SLAAC address is tentative until its own DAD passes.
    CHECK(!nic.local(global6) && nic.source(remote6) && nic.source(remote6).value() == self6);
    CHECK(nic.tick(2000));
    auto probe = icmp6(take(nic)); CHECK(nd(probe).target == global6 && wire::unspecified(probe.ip.source));
    CHECK(nic.tick(3000) && nic.local(global6) && nic.source(remote6).value() == global6 && nic.source(router6).value() == self6);
    CHECK(!nic.control());

    // Off-link IPv6 goes to the router learned from the advertisement.
    std::array<std::byte, 1514> out{};
    auto built = wire::build_udp<V6>(std::span{out}.subspan(14), global6, remote6, 1000, 53, 0, 64, payload); CHECK(built);
    auto sent = nic.output(out, built.value());
    CHECK(sent && sent.value() == 14 + 40 + 8 + 3 && eth(Frame(out.begin(), out.begin() + 65)).destination == router_mac);

    // The router asks for our global address: a solicited advertisement answers.
    auto ns = ip6(wire::multicast_mac(wire::solicited_node(global6)), router_mac, [&](std::span<std::byte> o) {
        return wire::build_neighbor_solicitation(o, router6, wire::solicited_node(global6), global6, router_mac); });
    nic.receive(ns, 3100);
    auto na = icmp6(take(nic)); auto message = nd(na);
    CHECK(message.kind == wire::NdKind::neighbor_advertisement && message.target == global6 && message.solicited && message.override_flag);
    CHECK(na.ip.source == global6 && na.ip.destination == router6 && na.link.destination == router_mac);
    CHECK(message.target_link && *message.target_link == self_mac);

    // Echo over IPv6 and UDP delivery to the global address.
    auto echo = ip6(self_mac, router_mac, [&](std::span<std::byte> o) {
        return wire::build_icmp<V6>(o, remote6, global6, 60, 0, wire::icmpv6::echo_request, 0, 0x00050006, payload); });
    nic.receive(echo, 3200);
    auto reply = icmp6(take(nic));
    CHECK(reply.message.type == wire::icmpv6::echo_reply && reply.message.rest == 0x00050006 && reply.ip.destination == remote6);
    auto datagram = ip6(self_mac, router_mac, [&](std::span<std::byte> o) {
        return wire::build_udp<V6>(o, remote6, global6, 5353, 9, 0, 60, payload); });
    auto received = nic.receive(datagram, 3300);
    CHECK(received.family == net::Delivery::ipv6 && received.protocol == wire::protocol_udp);
    nic.unreachable(received);
    auto error = icmp6(take(nic));
    CHECK(error.message.type == wire::icmpv6::unreachable && error.message.code == wire::icmpv6::port_unreachable);
    CHECK(error.ip.source == global6 && error.ip.destination == remote6);

    // Lifetimes: the router expires after 1800 s, the address deprecates at 14400 s.
    CHECK(nic.tick(2000 + 1800000) && !nic.routable(remote6));
    CHECK(nic.tick(2000 + 14400000));
    net::Ipv6AddressState state{};
    nic.each_ipv6([&](const net::Ipv6AddressSnapshot &a) { if (a.address == global6) state = a.state; });
    CHECK(state == net::Ipv6AddressState::deprecated && nic.local(global6));
    // A router lifetime of zero withdraws the default route at once.
    nic.receive(router_advertisement(1800, 86400, 14400), 2000 + 14400001);
    CHECK(nic.routable(remote6));
    nic.receive(router_advertisement(0, 86400, 14400), 2000 + 14400002);
    CHECK(!nic.routable(remote6));
    CHECK(nic.tick(2000 + 86400000 + 14400002));
    CHECK(!nic.local(global6));
}

void filtering()
{
    net::EthernetInterface nic;
    CHECK(nic.receive(udp4(self_mac, self4, 7), 0).family == net::Delivery::none);
    bring_up(nic);
    // A multicast group we did not join, an unknown EtherType, our own frames.
    auto other_group = ip6(wire::Mac{0x33, 0x33, 0, 0, 0, 0xfb}, router_mac, [&](std::span<std::byte> o) {
        return wire::build_udp<V6>(o, router6, {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfb}, 5353, 5353, 0, 255, payload); });
    CHECK(nic.receive(other_group, 0).family == net::Delivery::none);
    auto looped = udp4(self_mac, self4, 7); for (usize i = 0; i < 6; ++i) looped[6 + i] = std::byte{self_mac[i]};
    CHECK(nic.receive(looped, 0).family == net::Delivery::none);
    auto unknown = udp4(self_mac, self4, 7); wire::be16(unknown, 12, 0x88b5);
    CHECK(nic.receive(unknown, 0).family == net::Delivery::none);
    // Router solicitations go to all-routers, a group a host does not join.
    auto rs = ip6(wire::multicast_mac(wire::all_routers), host_mac, [&](std::span<std::byte> o) {
        return wire::build_router_solicitation(o, wire::link_local_from_mac(host_mac), host_mac); });
    CHECK(nic.receive(rs, 0).family == net::Delivery::none && !nic.control());
    // Replies to an unresolved gateway are held, not queued.
    for (int i = 0; i < 2; ++i) nic.receive(icmp4(router4, self4, wire::icmpv4::echo_request, 1, static_cast<u16>(i)), 0);
    CHECK(!nic.control() && nic.stats().held == 2);
    // A full control queue drops and counts: one ARP reply plus three echo replies fit.
    nic.receive(arp(1, wire::broadcast_mac, router_mac, router4, {}, self4), 0);
    for (int i = 0; i < 6; ++i) nic.receive(icmp4(router4, self4, wire::icmpv4::echo_request, 1, static_cast<u16>(i)), 0);
    CHECK(nic.stats().control_dropped == 3);
    usize queued = 0; while (nic.control()) { nic.pop_control(); ++queued; }
    CHECK(queued == 4 && nic.stats().control_sent == 4 + 2);
}
}

int main()
{
    configuration_and_dad();
    duplicate_address();
    arp_and_routed_output();
    ipv4_delivery_and_errors();
    ipv6_router_and_slaac();
    filtering();
    std::printf("PASS net interface scope=model checks=%zu\n", checks);
}
