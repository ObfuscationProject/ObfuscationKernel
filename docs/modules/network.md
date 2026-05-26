# Network

`ok::net::NetworkStack` is the early IPv4/UDP/TCP foundation for kernel network
debugging. It is fixed-capacity and freestanding: no heap allocation, no hosted
sockets, and no libc.

Implemented pieces:

- IPv4 address and checksum helpers.
- UDP loopback send/receive queues.
- TCP listen/connect state for loopback debug sessions.
- Per-protocol packet and byte counters exposed through the debug shell and the
  task manager.

The stack currently routes only local traffic. Real NIC support should add a
network driver that feeds Ethernet/IPv4 frames into this stack and transmits
frames produced by it. The next layers are ARP, ICMP, TCP sequence/window
tracking, retransmit timers, and a network-debug protocol on top of TCP or UDP.
