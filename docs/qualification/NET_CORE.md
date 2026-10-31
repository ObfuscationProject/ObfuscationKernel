# Network core

WT2.3 `feat/net-core`. Slice 1: the family-generic wire layer. Slice 2: the
neighbour cache and routing tables. Slice 3: the Ethernet interface and the
executor running through it. This note grows with each slice.

## Design

- `include/ok/net/ip.hpp` selects the network layer statically. The
  `AddressFamily` concept has `Ipv4Family` and `Ipv6Family` traits: address
  type, header size and builder, pseudo-header sum and ICMP protocol. UDP, TCP
  and ICMP parsers and builders take the family as an explicit template
  argument (`wire::udp<Ipv6Family>(packet)`). The IPv4 overloads in `wire.hpp`
  and `tcp_wire.hpp` are unchanged, and for IPv4 the generic builders emit
  the same bytes. The IPv6 parser skips hop-by-hop, destination-options and
  exhausted routing headers whose options may be skipped, and refuses
  fragments, as the IPv4 parser does.
- `include/ok/net/icmp.hpp` has ICMPv4/ICMPv6 messages, echo and error
  builders (an error quotes what fits in 576 or 1280 bytes) and the neighbour
  discovery messages with RFC 4861 validity checks: hop limit 255, code 0,
  option lengths, multicast targets and unspecified sources.
- `NeighbourCache<Address>` (`neighbour.hpp`) is one state machine for ARP and
  neighbour discovery: INCOMPLETE, REACHABLE, STALE, DELAY, PROBE and
  permanent entries. A request or a solicitation with a source address
  creates a STALE entry; an advertisement only updates a known one and obeys
  the solicited and override flags (RFC 4861 section 7.2.5). Unresolved
  neighbours hold at most three packets each; three unanswered probes delete
  the entry and its packets. Full caches replace the least recently used idle
  entry.
- `RouteTable<Address>` (`route.hpp`) selects the longest prefix, then the
  lowest metric, then the older route. Router-advertised routes expire.
- `EthernetInterface` (`interface.hpp`) owns both caches and both tables. It
  filters the link (its address, broadcast, all-nodes and the solicited-node
  groups of its IPv6 addresses), answers ARP requests for its address,
  answers echo requests, sends port unreachable for datagrams no socket takes
  (rate-limited: ten, then one per 100 ms; never for broadcast, multicast or
  ICMP), configures its link-local address through duplicate address
  detection, solicits routers, takes default routes, on-link prefixes, the
  MTU and SLAAC addresses from router advertisements, and frames egress
  packets through a route and a neighbour, holding them while resolution is
  pending. Frames it originates leave through a four-frame control queue.
- The kernel owns one interface. `NativeUdpService::configure` configures it
  from the OS link: the local address as a /24, the peer as default gateway
  and permanent neighbour, and IPv6 autoconfiguration. The shared Ethernet
  executor passes every received frame to the interface, delivers IPv4 TCP
  and UDP packets to their services, and transmits control frames, released
  packets and TCP segments in that order within its credit. A held or
  unroutable TCP segment counts as sent; retransmission recovers it. UDP and
  TCP bind/connect ask the interface for local addresses and routes, so any
  routable IPv4 peer is admitted, not only the static one.
- Without an attached interface `NativeUdpService` keeps the static peer
  profile; only the standalone `NativeUdpExecutor` (hosted tests) uses it.

## Not yet

- IPv6 transport sockets: IPv6 TCP and UDP packets reach the executor but no
  service accepts them (slice 5); ICMP errors are not delivered to sockets.
- The OS still supplies a static link; a configuration with prefix and
  gateway, and DHCP, belong to the paired OS worktree.
- Peers that answer only the exact UDP frame sequence (the OS
  `test_native_udp_socket.py` peer) now also see DAD and router solicitation
  frames at link start; the paired OS worktree skips link-control frames.

## Evidence

- Hosted: `scripts/test_net_core.py` runs `net_ip_tests` (golden vectors from
  an independent generator, generic/legacy byte identity, single-bit and
  truncation rejection), `net_neighbour_route_tests` and `net_interface_tests`
  with ASan/UBSan, and compiles `tests/host/net_core_compile.cpp` freestanding
  for all ten profiles. `tests/host/tcp_streams_tests.cpp` drives the
  executor through an attached interface.
- Kernel: `test_network_interface_model` (network/storage roadmap test
  points) runs ARP, echo over both families, neighbour solicitation and
  release of a held reply, and routed egress inside `kernel_main`. It is a
  model test executed on the target CPU, not a NIC or peer test.
