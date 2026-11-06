#include <ok/net/native_udp.hpp>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ok;
namespace wire = ok::net::wire;
using net::IpAddress;
using net::Family;

namespace
{
usize checks{};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "NET_UDP_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #condition); \
    std::abort(); } } while (false)

const wire::Mac self_mac{0x52, 0x54, 0, 0x12, 0x34, 0x56}, router_mac{0x52, 0x55, 0x0a, 0, 2, 2};
const wire::Ipv4 self4{10, 0, 2, 15}, router4{10, 0, 2, 2};
const wire::Ipv6 self6 = wire::link_local_from_mac(self_mac), router6 = wire::link_local_from_mac(router_mac);
const std::array<std::byte, 5> payload{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};

std::vector<std::byte> udp6(const wire::Ipv6 &source, const wire::Ipv6 &destination, u16 port)
{
    std::vector<std::byte> out(128);
    auto n = wire::build_udp<wire::Ipv6Family>(out, source, destination, 4000, port, 0, 64, payload); CHECK(n);
    out.resize(n.value()); return out;
}
std::vector<std::byte> udp4(const wire::Ipv4 &source, const wire::Ipv4 &destination, u16 port)
{
    std::vector<std::byte> out(128);
    auto n = wire::build_udp(out, source, destination, 4000, port, 7, 64, payload); CHECK(n);
    out.resize(n.value()); return out;
}

struct Fixture
{
    net::EthernetInterface nic{};
    net::NativeUdpService udp{};
    Fixture()
    {
        CHECK(udp.attach_interface(nic));
        CHECK(udp.configure({self_mac, router_mac, self4, router4}));
        CHECK(nic.configured() && nic.tick(0) && nic.tick(1000) && nic.local(self6));
        while (nic.control()) nic.pop_control();
    }
};

void ipv6_sockets()
{
    Fixture f;
    // A standalone service has no interface and refuses IPv6.
    net::NativeUdpService standalone;
    CHECK(standalone.open(Family::ipv6).status().code() == StatusCode::unsupported);
    auto socket = f.udp.open(Family::ipv6); CHECK(socket);
    CHECK(f.udp.family(socket.value()).value() == Family::ipv6);
    CHECK(!f.udp.bind(socket.value(), IpAddress{self4}, 5353));
    CHECK(f.udp.bind(socket.value(), IpAddress{wire::Ipv6{}}, 5353));
    auto binding = f.udp.binding(socket.value());
    CHECK(binding && binding.value().bound && binding.value().address.v6() && binding.value().address.unspecified());
    // Delivery by family and port; the IPv4 datagram to the same port is not for it.
    auto packet = udp6(router6, self6, 5353);
    auto queued = f.udp.deliver(packet); CHECK(queued && queued.value().kind == net::NativeUdpFrameKind::udp_queued);
    auto v4 = udp4(router4, self4, 5353);
    auto other = f.udp.deliver(v4); CHECK(other && other.value().kind == net::NativeUdpFrameKind::unbound);
    auto datagram = f.udp.peek(socket.value());
    CHECK(datagram && datagram.value().source == router6 && datagram.value().source_port == 4000 && datagram.value().payload.size() == 5);
    CHECK(f.udp.consume(socket.value()));
    auto unbound = udp6(router6, self6, 9);
    auto refused = f.udp.deliver(unbound); CHECK(refused && refused.value().kind == net::NativeUdpFrameKind::unbound);
    // A zero IPv6 checksum is invalid and dropped.
    auto zero = udp6(router6, self6, 5353); zero[46] = zero[47] = std::byte{};
    auto dropped = f.udp.deliver(zero); CHECK(dropped && dropped.value().kind == net::NativeUdpFrameKind::dropped);

    // Send: the router is unknown, so the datagram is held and resolution starts.
    std::array<std::byte, net::NativeUdpService::frame_capacity> frame{};
    CHECK(!f.udp.build_datagram(socket.value(), IpAddress{router4}, 53, payload, frame));
    auto built = f.udp.build_datagram(socket.value(), IpAddress{router6}, 53, payload, frame);
    CHECK(built && built.value() == 0 && f.nic.neighbours().pending() == 1);
    CHECK(f.nic.tick(1100) && f.nic.control());
    while (f.nic.control()) f.nic.pop_control();
    // The router answers; the held datagram leaves with its link address and our link-local source.
    std::array<std::byte, 128> advert{};
    auto na = wire::build_neighbor_advertisement(std::span{advert}.subspan(14), router6, self6, router6, true, true, true, router_mac);
    CHECK(na && wire::build_ethernet(advert, self_mac, router_mac, wire::ethertype_ipv6, na.value()));
    f.nic.receive({advert.data(), 14 + na.value()}, 1200);
    usize sent = 0;
    f.nic.flush([&](wire::Bytes out) {
        auto eth = wire::ethernet(out); CHECK(eth && eth.value().destination == router_mac);
        auto ip = wire::ipv6(eth.value().payload); CHECK(ip && ip.value().source == self6 && ip.value().destination == router6);
        auto d = wire::udp<wire::Ipv6Family>(ip.value()); CHECK(d && d.value().destination_port == 53 && d.value().source_port == 5353);
        ++sent; return true; });
    CHECK(sent == 1);
    // Now resolved: a second datagram is framed at once.
    built = f.udp.build_datagram(socket.value(), IpAddress{router6}, 53, payload, frame);
    CHECK(built && built.value() == 14 + 40 + 8 + 5);
    // Connect fixes the peer; the source address is selected for it.
    auto connected = f.udp.open(Family::ipv6); CHECK(connected);
    CHECK(f.udp.connect(connected.value(), IpAddress{router6}, 53));
    auto peer = f.udp.peer(connected.value());
    CHECK(peer && peer.value().associated && peer.value().address == router6);
    CHECK(f.udp.binding(connected.value()).value().address == self6);
    // Unroutable and multicast destinations are refused.
    wire::Ipv6 remote{0x20, 0x01, 0x0d, 0xb8}; remote[15] = 9;
    CHECK(!f.udp.connect(connected.value(), IpAddress{remote}, 53));
    CHECK(f.udp.disconnect(connected.value()));
    CHECK(f.udp.binding(connected.value()).value().address.v6());
}

void ipv4_through_interface()
{
    Fixture f;
    auto socket = f.udp.open(); CHECK(socket);
    CHECK(f.udp.bind(socket.value(), IpAddress{self4}, 6000));
    auto packet = udp4(router4, self4, 6000);
    auto queued = f.udp.deliver(packet); CHECK(queued && queued.value().kind == net::NativeUdpFrameKind::udp_queued);
    CHECK(f.udp.peek(socket.value()).value().source == router4);
    // Any routable IPv4 peer is admitted, through the gateway.
    std::array<std::byte, net::NativeUdpService::frame_capacity> frame{};
    auto built = f.udp.build_datagram(socket.value(), IpAddress{93, 184, 216, 34}, 53, payload, frame);
    CHECK(built && built.value() == 60);
    auto eth = wire::ethernet({frame.data(), built.value()}); CHECK(eth && eth.value().destination == router_mac);
    CHECK(!f.udp.build_datagram(socket.value(), IpAddress{224, 0, 0, 1}, 53, payload, frame));
}
}

int main()
{
    ipv6_sockets();
    ipv4_through_interface();
    std::printf("PASS net udp scope=model checks=%zu\n", checks);
}
