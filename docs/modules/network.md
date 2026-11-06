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
- Transports: `NativeUdpService` (datagram sockets of either family) and the
  TCP endpoint table (`tcp_transport.hpp`, `tcp_endpoints.hpp`);
  `NativeTcpEthernetExecutor` runs both over the interface from the kernel loop.
- Sockets: `AF_INET` and `AF_INET6` datagram and stream sockets in the native
  and Linux personalities. A socket keeps its `socket()` family; IPv6 sockets
  are v6-only. Linux records go through `ok/abi/linux_udp_address.hpp`
  (`sockaddr_in`, `sockaddr_in6` per profile); the native UAPI has
  `ok_sockaddr_in` and `ok_sockaddr_in6`.
- `NetworkStack`/`SocketTable` (`net.hpp`) remain the loopback debug stack of
  the rescue shell and task manager.

Limits: no dual-stack (IPv4-mapped addresses are refused), no IPv6 flow
labels or scope selection (one interface), no fragmentation, no forwarding. TCP sends a sliding window
under NewReno congestion control, reassembles out-of-order segments and
probes zero windows; it has no window scaling, timestamps or SACK. Design notes and evidence:
[NET_CORE](../qualification/NET_CORE.md).
