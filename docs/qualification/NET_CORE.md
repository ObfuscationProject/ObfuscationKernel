# Network core

WT2.3 `feat/net-core`. Slice 1: the family-generic wire layer. Slice 2: the
neighbour cache and routing tables. Slice 3: the Ethernet interface and the
executor running through it. Slice 4: general TCP. Slice 5: IPv6 transport
endpoints and `AF_INET6` sockets. This note grows with each slice.

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
  executor passes every received frame to the interface, delivers IPv4 and
  IPv6 TCP and UDP packets to their services, and transmits control frames, released
  packets and TCP segments in that order within its credit. A held or
  unroutable TCP segment counts as sent; retransmission recovers it. UDP and
  TCP bind/connect ask the interface for local addresses and routes, so any
  routable IPv4 peer is admitted, not only the static one.
- Without an attached interface `NativeUdpService` keeps the static peer
  profile; only the standalone `NativeUdpExecutor` (hosted tests) uses it.
- `tcp::Connection` (`tcp_transport.hpp`) sends several segments at once,
  bounded by the peer window and the congestion window of its static
  `Congestion` policy (`tcp_congestion.hpp`; the `CongestionControl` concept
  and `NewReno`: RFC 5681 initial window, slow start by at most one segment
  per ACK, congestion avoidance, RFC 6582 fast recovery). The send ring keeps
  every byte until it is acknowledged. One retransmission timer covers
  SND.UNA to SND.MAX; a timeout resends the first segment, backs the RTO off,
  collapses the window to one segment and resends the rest from SND.UNA as
  the window reopens (no RTT samples from resent data, Karn). The third
  duplicate ACK retransmits the first segment and enters recovery; partial
  ACKs retransmit the next hole. Segments beyond RCV.NXT are kept in the
  receive ring (four ranges) and joined when the gap fills, including a kept
  FIN. A zero window is probed with one byte after one RTO; a timed-out
  probe keeps the congestion window. The handshake, token protocol, RST and
  TIME_WAIT rules are unchanged.

## AF_INET6 sockets

- `socket(AF_INET6, SOCK_DGRAM|SOCK_STREAM)` opens a `NativeUdpService`
  socket or a `NativeTcpStreamService` stream of `Family::ipv6`; the stream
  records its family at open, so an unbound stream already refuses IPv4
  endpoints. `AF_INET6` is 10 on all ten Linux profiles
  (`udp_af_inet6` in `linux_udp_data.hpp`) and `OK_AF_INET6` natively.
- Records: `Ipv6SocketAddressCodec` reads and writes the 28-byte
  `sockaddr_in6` (family and scope_id in profile byte order, port and
  flowinfo in network order) and accepts the 24-byte RFC 2133 form as input.
  `encode_socket_name` selects `sockaddr_in` or `sockaddr_in6` by address
  family for getsockname, getpeername, recvfrom and accept; outputs report
  28 and truncate to the caller's capacity. Output flowinfo is 0 and scope_id
  is 1 for a link-local address, else 0; input flowinfo and scope_id are
  ignored. The native `ok_sockaddr_in6` has the same layout and requires
  exactly 28 bytes.
- IPv6 sockets behave like Linux sockets with `IPV6_V6ONLY` set. Linux
  personality: a record shorter than 24 bytes is EINVAL; bind and connect
  refuse a non-`AF_INET6` record with EAFNOSUPPORT; sendto with an
  `AF_INET` record is ENETUNREACH, with an unknown family EINVAL and with
  `AF_UNSPEC` uses the connected peer (`udpv6_sendmsg`). An IPv4-mapped
  address is EINVAL for bind and ENETUNREACH for connect and sendto. UDP
  `AF_UNSPEC` connect still disconnects. Native personality: a record of
  another size is EINVAL, another family EAFNOSUPPORT, mapped addresses as
  above.
- IPv4 sockets keep their record rules unchanged, including an `AF_INET6`
  record to an IPv4 socket (Linux UDP EAFNOSUPPORT, Linux TCP EOPNOTSUPP,
  native EINVAL).

## Not yet

- TCP: no window scaling, timestamps, SACK or delayed ACKs; the peer MSS
  bounds segments but there is no path MTU discovery.

- ICMP errors are not delivered to sockets. TCP getsockname/getpeername are
  still deferred (EOPNOTSUPP) for both families; their records would come
  from `encode_socket_name`.
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
- TCP: `tcp_window_tests` (in `test_net_core.py`) covers window growth,
  peer-window limits, fast retransmit and recovery, timeout go-back, Karn,
  reassembly with more ranges than records, zero-window probes and twelve
  transfers of 30 000 bytes between two connections over a channel that
  reorders and drops 0, 10 or 25 % of packets. The existing transport,
  endpoint and stream suites (`tests/host/tcp_streams_xmake.lua`) pass; the
  out-of-order case of `tcp_transport_tests` now expects the kept segment to
  join.
- Sockets (scope=model, hosted): `linux_udp_address_tests` (sockaddr_in6
  vectors and name records on all ten profiles), `linux_udp_receive_order_tests`
  and `linux_udp_operation_body_tests` (family rules, lengths, truncation and
  an IPv6 send, neighbour-resolved resend, receive and connect on all ten
  profiles), `native_udp_receive_tests` (native AF_INET6 bind and recvfrom
  through the runtime), `tcp_streams_tests` and `tcp_accept_transaction_tests`
  (stream family, IPv6 connect and accept, 28-byte accept records on ten
  profiles) and `tcp_immediate_runtime_tests` (both personalities: v6-only
  record rules, an IPv6 connect after neighbour discovery and an accept4
  over the hosted link). None of them executes a guest or a NIC.
- Kernel: `test_network_interface_model` (network/storage roadmap test
  points) runs ARP, echo over both families, neighbour solicitation and
  release of a held reply, and routed egress inside `kernel_main`. It is a
  model test executed on the target CPU, not a NIC or peer test.
