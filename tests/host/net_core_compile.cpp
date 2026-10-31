#include <ok/net/icmp.hpp>

// Isolated freestanding object checks, not packet delivery or native execution.
ok::Result<ok::usize> compile_net_core_reply(std::span<std::byte> output, ok::net::wire::Bytes packet)
{
    using namespace ok::net::wire;
    if (ip_version(packet) == 6)
    {
        auto ip = ipv6(packet); if (!ip) return ip.status();
        auto message = icmp<Ipv6Family>(ip.value()); if (!message) return message.status();
        if (message.value().type != icmpv6::echo_request)
        {
            auto nd = neighbor_discovery(ip.value(), message.value()); if (!nd) return nd.status();
            return build_neighbor_advertisement(output, nd.value().target, ip.value().source, nd.value().target,
                                                false, true, true, nd.value().source_link);
        }
        return build_icmp<Ipv6Family>(output, ip.value().destination, ip.value().source, 64, 0,
                                      icmpv6::echo_reply, 0, message.value().rest, message.value().body);
    }
    auto ip = ipv4(packet); if (!ip) return ip.status();
    if (ip.value().protocol == protocol_udp)
    {
        auto datagram = udp<Ipv4Family>(ip.value()); if (!datagram) return datagram.status();
        return build_icmp_error<Ipv4Family>(output, ip.value().destination, ip.value().source, 1,
                                            icmpv4::unreachable, icmpv4::port_unreachable, 0, packet);
    }
    auto message = icmp<Ipv4Family>(ip.value()); if (!message) return message.status();
    return build_icmp<Ipv4Family>(output, ip.value().destination, ip.value().source, 64, 0,
                                  icmpv4::echo_reply, 0, message.value().rest, message.value().body);
}
