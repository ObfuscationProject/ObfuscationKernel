# Connected UDP zero-destination send successor

The user authorized continued M0-M12 implementation. This private unexported
leaf starts from the complete admitted ABI54 Kernel1388 / OS678 composition.
Preserve that source, all existing executions, the twenty native rows, all ten
native profiles and all four supplied musl profiles. No installed SDK changes.

Linux udp_sendmsg rejects a zero destination port in its supplied-sockaddr
branch, but its connected-default branch uses the copied inet_dport, including
zero. Connect succeeds, getpeername returns ENOTCONN for that association, and
default send can emit a zero-port datagram. The existing Linux adapter already
implements the first three distinctions except transmission. Its explicit
destination validation must remain byte-identical.

Permit zero in the output-only wire builder and NativeUdpService frame builder.
Destination zero remains rejected by the ingress parser: no endpoint can bind
zero as a receiving port. Keep TTL, length, checksum, aliasing, MTU, route,
capture, user-copy, canonical OFD and extension submit validation intact. Do not
recheck association in the builder: the adapter snapshots the default peer
before payload copy, and reconnect/disconnect/descriptor reuse callbacks must
not redirect or invalidate that already selected destination. No layout, syscall
number, enum, scheduler, extension or SDK profile change is included. This leaf
is not eligible for SDK export before separate coordinated review/selection.

Controlled production changes: include/ok/net/wire.hpp and src/net/native_udp.cpp.
Update useful tests in network_wire_tests.cpp, native_udp_connected_tests.cpp,
native_udp_binding_tests.cpp, linux_udp_connected_capture_tests.cpp and
linux_udp_connected_runtime_tests.cpp. Update the connected hosted runner scope
and include the wire/binding/native regression targets. Add one current-slice
documentation file; leave the historical S6 inventory and every OS byte intact.

Hosted gates cover literal packet/checksum and unchanged output tails, invalid
TTL/capacity/aliasing, ingress destination-zero rejection, zero-length and MTU
payloads, explicit destination-zero refusal, EFAULT and TX failure ordering,
canonical binding/name retention, original OFD/peer snapshot across reconnect,
disconnect and descriptor reuse, direct calls on ten profiles and three existing
socketcall profiles. Run ordinary and ASan/UBSan xmake suites and all-ten
freestanding wire object checks. Record a separate local Linux loopback public
socket probe; this is host behavior, not target NIC execution. Preserve failures
and recipes in fresh attempts. Actual guest/wire, standalone kernel/OS SDK
consumer builds, IRQ networking, TCP/IPv6 and full M0-M12 remain separate gates.
