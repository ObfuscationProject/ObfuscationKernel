# Network

The kernel network stack is freestanding and fixed-capacity: no heap
allocation, no hosted sockets and no libc. The OS owns the NIC driver and
passes whole Ethernet frames across the extension's network calls.

Layers:

- Wire codecs (`ok/net/wire.hpp`, `tcp_wire.hpp`, `ip.hpp`, `icmp.hpp`):
  Ethernet, ARP, IPv4, IPv6 with skippable extension headers, UDP and TCP for
  both families, ICMPv4/ICMPv6 echo and errors, neighbour discovery. Families
  are selected statically through the `AddressFamily` concept.
- Neighbours and routes (`neighbour.hpp`, `route.hpp`): one RFC 4861 cache
  type for ARP and neighbour discovery, and a longest-prefix routing table
  per family.
- `EthernetInterface` (`interface.hpp`): link filtering, ARP and neighbour
  discovery replies, ICMP echo and port unreachable, IPv6 duplicate address
  detection, router solicitation and SLAAC, routed egress with held packets.
- Transports: `NativeUdpService` (IPv4 datagram sockets) and the TCP endpoint
  table (`tcp_transport.hpp`, `tcp_endpoints.hpp`); `NativeTcpEthernetExecutor`
  runs both over the interface from the kernel loop.
- `NetworkStack`/`SocketTable` (`net.hpp`) remain the loopback debug stack of
  the rescue shell and task manager.

Limits: IPv4 sockets only (IPv6 packets are answered at the ICMPv6/ND level),
one interface, no fragmentation, no forwarding. The TCP core keeps one
segment in flight. Design notes and evidence:
[NET_CORE](../qualification/NET_CORE.md).
