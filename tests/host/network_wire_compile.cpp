#include <ok/net/wire.hpp>

// Isolated freestanding object checks, not packet delivery or native execution.
ok::Result<ok::usize> compile_network_wire(std::span<std::byte> output,
                                         ok::net::wire::Bytes input)
{
    using namespace ok::net::wire;
    auto link = ethernet(input);
    if (!link) return link.status();
    if (link.value().type == 0x0806)
    {
        auto claim = arp(link.value().payload);
        if (!claim) return claim.status();
        return claim.value().operation;
    }
    auto ip = ipv4(link.value().payload);
    if (!ip) return ip.status();
    auto datagram = udp(ip.value());
    if (!datagram) return datagram.status();
    return build_udp(output, ip.value().destination, ip.value().source,
                     datagram.value().destination_port, datagram.value().source_port,
                     ip.value().identification, 64, datagram.value().payload);
}
