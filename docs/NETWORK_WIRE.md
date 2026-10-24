# Network wire bytes

`ok/net/wire.hpp` provides checked, borrowed views for untagged Ethernet,
Ethernet/IPv4 ARP requests and replies, IPv4 without options or fragments, and
IPv4 UDP. Its UDP builder emits DF, an IPv4 header checksum and a UDP
pseudo-header checksum; a computed zero UDP checksum is encoded as `0xffff`.
IPv4 UDP input may omit its checksum. IPv4 total length and UDP length trim
Ethernet padding and extra IP payload respectively.

These are allocation-free byte operations, not a protocol engine, neighbor
cache, routing table or socket implementation. The caller retains the input
buffer for the lifetime of every view and applies interface, source-address,
TTL and neighbor policy. Parsing ARP never learns a neighbor. Unsupported VLAN,
802.3 length frames, IPv4 options and fragments return `unsupported`; malformed
lengths and checksums return `invalid_argument`.

Field helpers are unchecked and require caller-sized spans. The checksum helper
operates on a complete buffer; it cannot stream across odd chunk boundaries
without external parity state. The builder requires payload to be disjoint from
emitted output bytes and validates capacity and overlap before changing output.
Payload in an unused output tail is permitted.

`tests/host/network_wire_tests.cpp` uses an independently calculated golden
packet and checksum accumulator, adversarial field mutations, prefix truncation,
alias failure atomicity, computed-zero checksum handling and deterministic
sanitizer fuzzing. `scripts/test_network_wire.py` builds these tests with xmake
and optionally compiles an isolated freestanding object for all ten profiles.
These are hosted model/object checks. Debug kernels also reach byte checks in
the existing network/storage suite through `kernel_main`; QEMU execution of
those checks does not establish network packet delivery.

Example with retained evidence and explicit compilers:

```sh
PYTHONDONTWRITEBYTECODE=1 xmake network-wire-test --output=/tmp/wire-model-v1 --sanitize --toolchain-root=/path/to/toolchains
```

Kernel SDK exports include this header and preserve architecture/mode/compiler
and complete artifact hash validation. Consumers must use that exported copy.
