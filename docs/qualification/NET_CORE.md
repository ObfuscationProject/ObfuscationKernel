# Network core

WT2.3 `feat/net-core`. Slice 1: the family-generic wire layer. Slice 2: the
neighbour cache and routing tables. Slice 3: the Ethernet interface and the
executor running through it. Slice 4: general TCP. Slice 5: IPv6 transport
endpoints and `AF_INET6` sockets. Slice 6: readiness through wait queues.
Slice 7: the link probe and its QEMU evidence. Follow-up
`net-socket-gaps`: ICMP errors to sockets, socket poll and select on wait
queues, TCP socket names. This note grows with each slice.

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
- `net::IpAddress` (`address.hpp`) holds either family. `tcp::Endpoint` uses
  it: the connection and the endpoint table parse and build IPv4 and IPv6
  segments and refuse mixed endpoints. `NativeUdpService` sockets carry a
  family fixed at open; IPv6 sockets send through the interface with a
  source selected for the destination, and the executor delivers IPv6 TCP and
  UDP packets.
- `sched::WaitQueue` enlists registrations the scheduler armed (also in
  several queues) and keeps each entry's ticket, so a wait completed
  elsewhere leaves only a stale entry; `notify` offers live waiters to the
  condition's owner. The TCP endpoint table records which slots' readiness
  moved (`take_changed`); `TcpStreamWaits` enlists each waiter in its
  endpoint's queue and re-evaluates readiness only for changed endpoints.
  Descriptor, lost-thread and deadline checks still run on every pass.
- `KernelExtension::network_interface` lets the distribution configure the
  interface (address, prefix, gateway resolved through ARP, IPv6);
  `Kernel::activate_network` prefers it to the static link and starts the
  network clock. `KernelExtension::network_probe` asks for `LinkProbe`
  (`probe.hpp`): at the first network pass the kernel brings the link up,
  pings the IPv4 gateway and the IPv6 router, reports the autoconfigured
  addresses and exchanges bytes with a TCP echo service, printing
  `OK_NET_LINK`, `OK_NET_ARP`, `OK_NET_PING`, `OK_NET_IPV6`, `OK_NET_TCP` and
  `OK_NET_PROBE`.

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

## Socket errors, names and poll

- ICMP errors reach the transport whose packet they quote.
  `classify_icmp_error` gives each error a kind and a hard/soft class
  (Linux `icmp_err_convert` and `icmpv6_err_convert`; IPv4 protocol
  unreachable and codes 7 and 8 map to the nearest kind). `icmp_quote` reads
  the quoted header and the first eight transport bytes. The executor routes
  each error by quoted protocol and endpoints.
  - TCP follows Linux `tcp_v4_err`. The quoted sequence must lie in
    [SND.UNA, SND.MAX). A hard error ends a handshake with that failure. A
    soft error, or a hard error on an established connection, is remembered
    and named if the connection later times out, and acknowledged progress
    forgets it. Fragmentation needed and packet too big lower the peer MSS
    (floored at 576 and 1280) and resend from SND.UNA. Failures map to
    ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, EACCES and EPROTO, with per-profile
    numbers in `linux_tcp_data.hpp`.
  - UDP follows Linux `__udp4_lib_err`. Only a hard port-unreachable on a
    connected socket is kept. It wakes the socket's waiters, makes poll report
    POLLERR, and is reported once as ECONNREFUSED by the next receive (before
    any queued datagram) or send.
- Poll and select over TCP streams have a readiness provider
  (`tcp_stream_poll.hpp`, Linux `tcp_poll`):
  - an unconnected stream reports POLLOUT|POLLHUP;
  - a listener is readable while a connection waits;
  - a handshake in progress reports nothing;
  - a closed or failed connection reports POLLIN|POLLOUT|POLLHUP, plus
    POLLERR if it failed.

  Before this, such a poll failed with ENOSYS.
- A poll or select whose descriptors are all sockets sleeps on their wait
  queues. These are the TCP endpoint queues and the UDP receive queues;
  a UDP descriptor asking for output is excluded because transmit credit
  signals no queue. Such a poll is evaluated only after a queue signals it,
  or at its timeout. Any other descriptor keeps the per-pass rescan. UDP
  receive waits wake from their socket queue only, and a failed executor
  wakes all of them.
- A failure is reported once, as Linux `sock_error` clears `sk_err`.
  - TCP: the next read, write, connect or `getsockopt(SO_ERROR)` takes it.
    Afterwards the closed connection reads EOF, writes EPIPE and polls
    without POLLERR. `SO_ERROR` also takes a remembered soft ICMP error
    (`sk_err_soft`).
  - UDP: `SO_ERROR` takes a pending refusal like a receive or send would.
    getsockopt on a UDP socket used to be ENOTSOCK.
  - `SO_TYPE` reports SOCK_STREAM or SOCK_DGRAM. Option lengths follow
    `sock_getsockopt`: a negative length is EINVAL, at most four bytes are
    copied, and the copied size is written back.
- TCP getsockname reports the bound name (an unbound stream reports the
  family's unspecified address). getpeername reports the peer of a connected
  stream and ENOTCONN in SYN_SENT, CLOSE and LISTEN (Linux `inet_getname`).
  Records come from `encode_socket_name`; the length rules match the UDP
  calls (truncate to the capacity, report the full size, a negative length
  is EINVAL).

## Not yet

- TCP: no window scaling, timestamps, SACK or delayed ACKs. Path MTU
  discovery only lowers the MSS on an ICMP report (segments carry DF); the
  MSS is never raised again and there is no black-hole detection.

- An ICMP error is not checked against the packet's original destination
  beyond the quoted endpoints. There is no `IP_RECVERR` error queue, and
  getsockopt knows only `SO_ERROR` and `SO_TYPE` (other options are
  EOPNOTSUPP rather than ENOPROTOOPT; setsockopt stays unsupported). A
  second connect after a failed one whose error was reported is EISCONN
  rather than a new attempt.
- The OS's default image keeps the static link (gateway as a permanent
  neighbour); only its `net_probe` image uses the routed interface. DHCP and
  a configurable address belong to OS services.
- QEMU user networking is unavailable with the pinned QEMU (no slirp), so the
  device evidence uses a scripted peer, not a third-party stack.

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
- IPv6 transport: `tcp_window_tests` runs four lossy transfers between IPv6
  endpoints and accepts an IPv6 connection through the endpoint table;
  `net_udp_tests` runs both socket families through the UDP service and the
  interface (held, resolved and routed sends, family mismatches).
- Wait queues: `irq_wait_time_tests` covers enlisting, stale entries,
  `notify` and multi-queue enlistment; `tcp_streams_tests` checks that a
  pass without a readiness change evaluates no waiter and that a segment's
  edge evaluates it once.
- Socket errors and poll (hosted, scope=model):
  - `tcp_window_tests` covers ICMP errors built against the connection's own
    segments, both families: hard and soft errors, out-of-window quotes,
    progress forgetting a soft error, the MTU floor, and the IPv4
    classification table.
  - `net_udp_tests` covers the connected-only, report-once UDP error.
  - `tcp_streams_tests` routes errors through the executor and wakes a
    connect waiter.
  - `tcp_immediate_runtime_tests` covers ECONNREFUSED, EHOSTUNREACH and
    ENETUNREACH from an ICMP-refused connect. Each is reported once, by a
    read, `SO_ERROR` or a second connect, and is then followed by EOF, EPIPE
    and no POLLERR. It also covers TCP names, `SO_TYPE` and option lengths,
    UDP sendto and `SO_ERROR` after a refusal (both personalities), the TCP
    poll states, and a blocking poll that stays unevaluated over idle passes
    until data arrives.
  - `native_udp_receive_tests` covers a blocked receiver woken with
    ECONNREFUSED, POLLERR and the same gating for UDP.
  - `scripts/test_net_runtime.py` builds and runs these runtime suites with
    ASan/UBSan, one binary per test.
- Probe: `net_probe_tests` runs `LinkProbe` against a scripted slirp-like
  gateway (exact marker text), also with ten percent TCP loss each way.
- QEMU device execution (paired OS `feat/net-services`):
  `scripts/test_net_probe.py` boots a `net_probe=y` distribution image on a
  socket netdev against the OS's scripted peer (`tools/net_probe_peer.py`:
  ARP, ICMPv4/ICMPv6 echo, router and neighbour advertisements, a
  retransmitting TCP echo). On x86_64 the guest resolved the gateway, pinged
  it (21 ms), autoconfigured `2001:db8:1:0:11:22ff:fe33:4455`, pinged the
  router and echoed 8192 TCP bytes; with ten percent TCP loss the transfer
  completed after retransmissions with the congestion window collapsed.
  At kernel 956c7be (OS 6f1a208) the same run passes on all ten profiles,
  clean and with ten percent TCP loss, after `xmake test` on each; the
  native UDP socket test (x86_64) reports its blocking receive enlisted and
  evaluated once (`OK_NET_WAITS`). Receipts: ObfuscationEvidence
  `receipts/ObfuscationKernel@956c7be/feat/net-core/` (`device-probe-matrix`,
  `hosted`).
