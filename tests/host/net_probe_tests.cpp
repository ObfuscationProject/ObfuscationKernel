#include <ok/net/probe.hpp>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

using namespace ok;
namespace wire = ok::net::wire;
using V4 = wire::Ipv4Family;
using V6 = wire::Ipv6Family;

namespace
{
usize checks{};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "NET_PROBE_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #condition); \
    std::abort(); } } while (false)

using Frame = std::vector<std::byte>;
const wire::Mac self_mac{0x52, 0x54, 0, 0x12, 0x34, 0x56}, gateway_mac{0x52, 0x55, 0x0a, 0, 2, 2};
const wire::Ipv4 self4{10, 0, 2, 15}, gateway4{10, 0, 2, 2}, echo4{10, 0, 2, 100};
const wire::Ipv6 router6 = wire::link_local_from_mac(gateway_mac);
const wire::Ipv6 prefix{0xfe, 0xc0};

// A slirp-like gateway: ARP for its addresses, echo, router advertisements,
// neighbour advertisements, and a TCP echo service at echo4:7.
struct Gateway
{
    std::deque<Frame> to_guest;
    net::tcp::Connection<8192, 8192, 1460> echo{};
    bool listening{};
    u32 loss_seed{};
    usize lost{};
    template <class Build> void send(const wire::Mac &destination, u16 type, Build &&build)
    {
        Frame out(1514);
        auto length = build(std::span{out}.subspan(14)); CHECK(length);
        CHECK(wire::build_ethernet(out, destination, gateway_mac, type, length.value()));
        out.resize(14 + length.value() < 60 ? 60 : 14 + length.value());
        to_guest.push_back(out);
    }
    bool drop()
    {
        if (!loss_seed) return false;
        loss_seed = loss_seed * 1664525u + 1013904223u;
        if ((loss_seed >> 8) % 100 >= 10) return false;
        ++lost; return true;
    }
    void receive(wire::Bytes frame, u64 now)
    {
        auto eth = wire::ethernet(frame); CHECK(eth);
        const auto &e = eth.value();
        if (e.type == wire::ethertype_arp)
        {
            auto arp = wire::arp(e.payload); CHECK(arp);
            const auto &a = arp.value();
            if (a.operation == 1 && (a.target_ip == gateway4 || a.target_ip == echo4))
                send(a.sender_mac, wire::ethertype_arp, [&](std::span<std::byte> o) {
                    return wire::build_arp(o, 2, gateway_mac, a.target_ip, a.sender_mac, a.sender_ip); });
            return;
        }
        if (e.type == wire::ethertype_ipv4)
        {
            CHECK(e.destination == gateway_mac);
            auto ip = wire::ipv4(e.payload); CHECK(ip);
            if (ip.value().protocol == wire::protocol_icmp)
            {
                auto m = wire::icmp<V4>(ip.value()); CHECK(m && m.value().type == wire::icmpv4::echo_request);
                CHECK(ip.value().destination == gateway4);
                send(self_mac, wire::ethertype_ipv4, [&](std::span<std::byte> o) {
                    return wire::build_icmp<V4>(o, gateway4, self4, 64, 1, wire::icmpv4::echo_reply, 0, m.value().rest, m.value().body); });
                return;
            }
            CHECK(ip.value().protocol == wire::protocol_tcp && ip.value().destination == echo4);
            if (drop()) return;
            if (!listening) { CHECK(echo.passive_open({echo4, 7}, 777, now)); listening = true; }
            const wire::Bytes packet = e.payload.first(wire::be16(e.payload, 2));
            CHECK(echo.receive(packet, now));
            return;
        }
        CHECK(e.type == wire::ethertype_ipv6);
        auto ip = wire::ipv6(e.payload); CHECK(ip);
        auto m = wire::icmp<V6>(ip.value()); CHECK(m);
        const auto &message = m.value();
        if (message.type == wire::icmpv6::router_solicitation)
        {
            std::array<std::byte, 8 + 8 + 32> body{};
            body[8] = std::byte{1}; body[9] = std::byte{1};
            for (usize i = 0; i < 6; ++i) body[10 + i] = std::byte{gateway_mac[i]};
            body[16] = std::byte{3}; body[17] = std::byte{4}; body[18] = std::byte{64}; body[19] = std::byte{0xc0};
            wire::tcp_detail::be32(body, 20, 86400); wire::tcp_detail::be32(body, 24, 14400);
            for (usize i = 0; i < 16; ++i) body[32 + i] = std::byte{prefix[i]};
            send(wire::multicast_mac(wire::all_nodes), wire::ethertype_ipv6, [&](std::span<std::byte> o) {
                return wire::build_icmp<V6>(o, router6, wire::all_nodes, 255, 0, wire::icmpv6::router_advertisement, 0,
                                            (u32{64} << 24) | 1800u, body); });
        }
        else if (message.type == wire::icmpv6::neighbor_solicitation)
        {
            auto nd = wire::neighbor_discovery(ip.value(), message); CHECK(nd);
            if (nd.value().target != router6 || wire::unspecified(ip.value().source)) return;
            send(self_mac, wire::ethertype_ipv6, [&](std::span<std::byte> o) {
                return wire::build_neighbor_advertisement(o, router6, ip.value().source, router6, true, true, true, gateway_mac); });
        }
        else if (message.type == wire::icmpv6::echo_request)
        {
            CHECK(ip.value().destination == router6);
            send(self_mac, wire::ethertype_ipv6, [&](std::span<std::byte> o) {
                return wire::build_icmp<V6>(o, router6, ip.value().source, 64, 0, wire::icmpv6::echo_reply, 0, message.rest, message.body); });
        }
    }
    void serve(u64 now)
    {
        if (!listening) return;
        std::array<std::byte, 4096> data{};
        for (;;)
        {
            auto got = echo.read(data);
            if (!got || !got.value()) break;
            usize written = 0;
            while (written < got.value())
            {
                auto w = echo.write(wire::Bytes{data.data() + written, got.value() - written});
                if (!w || !w.value()) break;
                written += w.value();
            }
            CHECK(written == got.value());
        }
        for (;;)
        {
            Frame out(1514);
            auto prepared = echo.prepare(std::span{out}.subspan(14), now); CHECK(prepared);
            if (!prepared.value()) break;
            CHECK(echo.complete_transmit(prepared.value()->token, true, now));
            if (drop()) continue;
            CHECK(wire::build_ethernet(out, self_mac, gateway_mac, wire::ethertype_ipv4, prepared.value()->length));
            out.resize(14 + prepared.value()->length < 60 ? 60 : 14 + prepared.value()->length);
            to_guest.push_back(out);
        }
    }
};

struct Iss
{
    Result<u32> operator()(net::tcp::Endpoint, net::tcp::Endpoint) { return u32{0x10000}; }
};

struct Run
{
    std::string serial;
    net::ProbeResult result;
    usize lost{};
};

Run run(u32 loss_seed, u16 bytes, bool tcp = true)
{
    net::EthernetInterface nic;
    CHECK(nic.configure({self_mac, 1500, net::Ipv4Configuration{self4, 24, gateway4, {}}, true}));
    net::NativeTcpStreamService service{net::tcp::TableIdentity{91}};
    net::LinkProbe probe;
    net::ProbePlan plan{};
    if (tcp) plan.tcp_echo = net::tcp::Endpoint{echo4, 7};
    plan.tcp_bytes = bytes;
    probe.arm(plan);
    Gateway gateway; gateway.loss_seed = loss_seed;
    Iss iss;
    Run out;
    std::array<std::byte, 1514> frame{};
    for (u64 now = 0; now < 70000 && !probe.result().finished; now += 10)
    {
        CHECK(service.endpoints().tick(now) && nic.tick(now));
        while (!gateway.to_guest.empty())
        {
            const auto f = gateway.to_guest.front(); gateway.to_guest.pop_front();
            auto received = nic.receive(f, now);
            if (received.family == net::Delivery::ipv4 && received.protocol == wire::protocol_tcp)
                CHECK(service.endpoints().ingest(received.packet, now, iss));
        }
        probe.step(nic, service, now, iss, [&](std::string_view text) { out.serial.append(text); });
        while (auto control = nic.control()) { gateway.receive(*control, now); nic.pop_control(); }
        nic.flush([&](wire::Bytes f) { gateway.receive(f, now); return true; });
        for (;;)
        {
            auto prepared = service.endpoints().prepare(std::span{frame}.subspan(14), now); CHECK(prepared);
            if (!prepared.value()) break;
            auto framed = nic.output(frame, prepared.value()->length);
            CHECK(service.endpoints().complete(prepared.value()->ticket, true, now));
            if (framed && framed.value()) gateway.receive({frame.data(), framed.value()}, now);
        }
        gateway.serve(now);
    }
    out.result = probe.result(); out.lost = gateway.lost;
    return out;
}

bool has(const std::string &serial, const char *text) { return serial.find(text) != std::string::npos; }

void clean_link()
{
    auto r = run(0, 4096);
    CHECK(r.result.finished && r.result.passed);
    CHECK(has(r.serial, "OK_NET_LINK scope=hardware mac=52:54:00:12:34:56 ipv4=10.0.2.15/24 gateway=10.0.2.2 ipv6=on\n"));
    CHECK(has(r.serial, "OK_NET_ARP scope=hardware gateway=10.0.2.2 mac=52:55:0a:00:02:02\n"));
    CHECK(has(r.serial, "OK_NET_PING scope=hardware family=4 peer=10.0.2.2 sequence=1 rtt_ms="));
    CHECK(has(r.serial, "OK_NET_IPV6 scope=hardware link_local=fe80::5054:ff:fe12:3456 global=fec0::5054:ff:fe12:3456 router=fe80::5055:aff:fe00:202 mtu=1500\n"));
    CHECK(has(r.serial, "OK_NET_PING scope=hardware family=6 peer=fe80::5055:aff:fe00:202 sequence=2 rtt_ms="));
    CHECK(has(r.serial, "OK_NET_TCP scope=hardware peer=10.0.2.100:7 bytes=4096 echoed=4096 cwnd="));
    CHECK(has(r.serial, "OK_NET_PROBE scope=hardware result=pass link=1 arp=1 ping4=1 ipv6=1 ping6=1 tcp=1\n"));
}

void lossy_link()
{
    // Ten percent of TCP segments lost in each direction: retransmission recovers them.
    for (u32 seed : {3u, 17u, 41u})
    {
        auto r = run(seed, 16384);
        CHECK(r.lost > 0 && r.result.passed && has(r.serial, "bytes=16384 echoed=16384"));
    }
}

void no_echo_service_fails()
{
    // Without a TCP plan the probe still covers ARP, ping and IPv6.
    auto r = run(0, 0, false);
    CHECK(r.result.passed && !r.result.tcp && has(r.serial, "result=pass link=1 arp=1 ping4=1 ipv6=1 ping6=1 tcp=0"));
}
}

int main()
{
    clean_link();
    lossy_link();
    no_echo_service_fails();
    std::printf("PASS net probe scope=model checks=%zu\n", checks);
}
