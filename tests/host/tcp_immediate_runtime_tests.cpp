// Real configured runtime/origin/OFD dispatch with explicit hosted trap and NIC models.
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_udp_receive_model.hpp"
#include <algorithm>
#include <source_location>

namespace
{
usize checks{};
void expect(bool value, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "TCP_IMMEDIATE_RUNTIME_FAIL line=%u checks=%zu\n", where.line(), checks); std::abort(); }
}
constexpr net::NativeUdpLink link{{2, 0x11, 0x22, 0x33, 0x44, 0x55},
    {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}, {192, 0, 2, 2}, {192, 0, 2, 1}};
constexpr uptr address_pointer = hw::user_begin + 768;
constexpr uptr payload_pointer = hw::user_begin + 128;
using Peer = net::tcp::Connection<1024, 1024, 536>;
class Entropy final : public driver::EntropySource
{
  public:
    Status fill(std::span<std::byte> output) override
    { for (auto &byte : output) byte = std::byte{++value}; return {}; }
    u8 value{};
};
class Link final : public KernelExtension
{
  public:
    Result<net::NativeUdpLink> network_link(Kernel &) override { ++queries; return link; }
    Result<net::NativeUdpTransmitState> network_poll_transmit(Kernel &, usize budget) override
    {
        expect(budget == 8);
        return failed ? Result<net::NativeUdpTransmitState>{Status::io_error("hosted NIC failure")}
                      : Result<net::NativeUdpTransmitState>{net::NativeUdpTransmitState{0, 8}};
    }
    Result<usize> network_receive(Kernel &, std::span<std::byte> output) override
    {
        ++receives;
        if (!incoming_count) return Status::would_block();
        const auto size = incoming_lengths[incoming_head]; expect(size <= output.size());
        std::copy_n(incoming[incoming_head].begin(), size, output.begin());
        incoming_head = (incoming_head + 1) % incoming.size(); --incoming_count; return size;
    }
    Status network_try_transmit(Kernel &, std::span<const std::byte> frame) override
    {
        ++submissions;
        expect(frame.size() <= outgoing[0].size() && count < outgoing.size());
        const usize slot = (head + count) % outgoing.size();
        std::copy(frame.begin(), frame.end(), outgoing[slot].begin()); lengths[slot] = frame.size(); ++count; return {};
    }
    // Immediate sends (native UDP sendto) take the same TX queue.
    Status network_transmit(Kernel &kernel, std::span<const std::byte> frame) override
    { return network_try_transmit(kernel, frame); }
    void receive_peer(Peer &peer)
    {
        if (!count) std::fprintf(stderr, "TCP_IMMEDIATE_PEER_EMPTY submissions=%zu receives=%zu peer_state=%u\n",
            submissions, receives, static_cast<unsigned>(peer.snapshot().state));
        expect(count != 0);
        while (count)
        {
            auto ethernet = net::wire::ethernet({outgoing[head].data(), lengths[head]}); CHECK(ethernet);
            auto accepted = peer.receive(ethernet.value().payload, mock::now / 1000000); CHECK(accepted);
            head = (head + 1) % outgoing.size(); --count;
        }
    }
    void send_peer(Peer &peer, u16 type = 0x0800)
    {
        expect(!incoming_count);
        // ACK and data/FIN are distinct prepared packets. Retain every accepted
        // copied frame, matching the bounded NIC queue contract.
        while (incoming_count < incoming.size())
        {
            const usize slot = (incoming_head + incoming_count) % incoming.size();
            auto &bytes = incoming[slot];
            auto prepared = peer.prepare(std::span{bytes}.subspan(14), mock::now / 1000000); CHECK(prepared);
            if (!prepared.value()) break;
            const auto packet = *prepared.value();
            for (usize at = 0; at < 6; ++at)
            { bytes[at] = std::byte{link.local_mac[at]}; bytes[at + 6] = std::byte{link.peer_mac[at]}; }
            net::wire::be16(bytes, 12, type); incoming_lengths[slot] = packet.length + 14; ++incoming_count;
            CHECK(peer.complete_transmit(packet.token, true, mock::now / 1000000));
        }
        expect(incoming_count != 0);
    }
    std::array<std::array<std::byte, 1514>, 8> incoming{};
    std::array<usize, 8> incoming_lengths{};
    std::array<std::array<std::byte, 1514>, 8> outgoing{};
    std::array<usize, 8> lengths{};
    usize queries{}, receives{}, submissions{}, incoming_head{}, incoming_count{}, head{}, count{};
    bool failed{};
};
Entropy entropy;
Link extension;
struct Group { u64 pid{}; bool linux{}; };
Group create(Kernel &kernel, bool linux)
{
    auto made = kernel.scheduler().create_user_process("tcp-immediate-host",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 4096})); CHECK(made);
    auto *process = kernel.scheduler().find(made.value());
    process->resources().personality = linux ? sched::UserPersonality::linux : sched::UserPersonality::native;
    process->resources().native_space = {0x100000 + made.value() * 4096, static_cast<u32>(made.value() % 16), 1};
    auto &thread = process->threads()[0]; CHECK(native::prepare_native_thread(thread)); thread.native_active = true;
    CHECK(kernel.scheduler().set_runnable(made.value()));
    CHECK(kernel.scheduler().activate_thread(made.value(), made.value())); CHECK(kernel.posix().activate(made.value()));
    native::runtime_kernel = &kernel; native::started = true; return {made.value(), linux};
}
void retire(Kernel &kernel, Group group)
{
    native::release_process(kernel, group.pid);
    CHECK(kernel.scheduler().exit_process(group.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::runtime_status().ok() && native::entry_stacks.accounting().leased_slots == 0);
}
i64 invoke(Kernel &kernel, Group group, u32 number, std::array<uptr, 6> arguments)
{
    auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    auto *caller = kernel.scheduler().find_thread(group.pid, group.pid); CHECK(caller);
    CHECK(kernel.scheduler().activate_thread(group.pid, group.pid)); CHECK(kernel.posix().activate(group.pid));
    hw::UserFrame frame{}; frame.pc = hw::user_begin + 256; frame.sp = hw::user_begin + 4096; frame.registers[14] = number;
    constexpr std::array<usize, 6> slots{9, 10, 11, 5, 7, 6};
    for (usize at = 0; at < slots.size(); ++at) frame.registers[slots[at]] = arguments[at];
    caller->native_frame = frame; caller->syscall_pending = false;
    mock::entry_trap = {hw::TrapKind::syscall, 256}; native::last_pid = native::last_tid = 0;
    const auto entries = mock::entries; expect(native::step(kernel)); expect(mock::entries == entries + 1);
    expect(native::runtime_status().ok() && !caller->syscall_pending);
    expect(!native::TcpImmediateAccess<lu::Profile::x86_64>::acquire(native::syscall_origins, *process, *caller));
    return static_cast<i64>(caller->native_frame.registers[14]);
}
constexpr uptr name_length_pointer = hw::user_begin + 1000;
// A name call's copied record and reported length (x86_64: the native and
// Linux records share family, port and address offsets).
struct Name { i64 result{}; u32 length{}; std::array<u8, 28> record{}; };
Name name_call(Kernel &kernel, Group group, u32 number, i32 fd, i32 capacity)
{
    std::memset(mock::memory.data() + 768, 0xa5, 28);
    std::memcpy(mock::memory.data() + 1000, &capacity, sizeof(capacity));
    Name out; out.result = invoke(kernel, group, number, {static_cast<uptr>(fd), address_pointer, name_length_pointer});
    std::memcpy(&out.length, mock::memory.data() + 1000, sizeof(out.length));
    std::memcpy(out.record.data(), mock::memory.data() + 768, out.record.size());
    return out;
}
// A zero-timeout poll of one descriptor: its revents, or the error.
i64 poll_now(Kernel &kernel, Group group, i32 fd, i16 events)
{
    ok_pollfd entry{fd, events, 0}; std::memcpy(mock::memory.data() + 896, &entry, sizeof(entry));
    const auto ready = invoke(kernel, group, 7, {hw::user_begin + 896, 1, 0});
    std::memcpy(&entry, mock::memory.data() + 896, sizeof(entry));
    return ready < 0 ? ready : static_cast<u16>(entry.revents);
}
// getsockopt(fd, level, option) into a four-byte value with `capacity`.
struct Option { i64 result{}; u32 length{}; u32 value{}; };
Option option_call(Kernel &kernel, Group group, i32 fd, u32 level, u32 option, i32 capacity = 4)
{
    std::memset(mock::memory.data() + 768, 0xa5, 4);
    std::memcpy(mock::memory.data() + 1000, &capacity, sizeof(capacity));
    Option out; out.result = invoke(kernel, group, 55, {static_cast<uptr>(fd), level, option, address_pointer, name_length_pointer});
    std::memcpy(&out.length, mock::memory.data() + 1000, sizeof(out.length));
    std::memcpy(&out.value, mock::memory.data() + 768, sizeof(out.value));
    return out;
}
void address(net::wire::Ipv4 ip, u16 port)
{
    std::array<u8, 16> encoded{}; CHECK(lu::UdpWire<lu::Profile::x86_64>::encode_address(encoded, {2, port, ip}));
    std::memcpy(mock::memory.data() + 768, encoded.data(), encoded.size());
}
net::TcpStreamHandle stream(Kernel &kernel, Group group, i32 fd)
{
    const auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    const auto *slot = process->fd_table().entry(fd); CHECK(slot && slot->used);
    const auto *description = kernel.scheduler().open_description(slot->description); CHECK(description);
    return description->native_tcp_handle;
}
void address6(u16 family, const net::wire::Ipv6 &ip, u16 port)
{
    std::array<u8, 28> encoded{};
    CHECK(lu::Ipv6SocketAddressCodec<lu::Profile::x86_64>::encode(encoded, {family, port, 0x01020304u, ip, 9}));
    std::memcpy(mock::memory.data() + 768, encoded.data(), encoded.size());
}
// Feeds the peer only TCP segments; interface frames (DAD, ND) are dropped.
void receive_tcp6(Peer &peer)
{
    while (extension.count)
    {
        auto &frame = extension.outgoing[extension.head];
        auto ethernet = net::wire::ethernet({frame.data(), extension.lengths[extension.head]}); CHECK(ethernet);
        if (ethernet.value().type == net::wire::ethertype_ipv6)
        {
            auto ip = net::wire::ipv6(ethernet.value().payload); CHECK(ip);
            if (ip.value().protocol == net::wire::protocol_tcp)
                CHECK(peer.receive(ethernet.value().payload, mock::now / 1000000));
        }
        extension.head = (extension.head + 1) % extension.outgoing.size(); --extension.count;
    }
}
void poll6(Kernel &kernel, Peer *peer = nullptr)
{
    CHECK(kernel.poll_network());
    if (peer) receive_tcp6(*peer); else { extension.head = extension.count = 0; }
}
// AF_INET6 streams through the configured runtime: v6-only record rules for
// both personalities, then a connect and an accept over the hosted link.
void ipv6_exercise(Kernel &kernel, bool linux, u16 port)
{
    namespace wire = net::wire;
    const auto self = wire::link_local_from_mac(link.local_mac), router = wire::link_local_from_mac(link.peer_mac);
    const wire::Ipv6 remote{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9};
    const wire::Ipv6 mapped{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 2};
    auto group = create(kernel, linux);
    const auto fd = invoke(kernel, group, 41, {10, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}); expect(fd >= 3);
    const auto handle = stream(kernel, group, static_cast<i32>(fd));
    expect(kernel.native_tcp().snapshot(handle).value().family == net::Family::ipv6);
    // Duplicate address detection settles the link-local address first.
    for (usize step = 0; step < 4 && !kernel.network_interface().local(self); ++step)
    { mock::now += 1000000000; poll6(kernel); }
    expect(kernel.network_interface().local(self));
    const auto at = static_cast<uptr>(fd);
    address6(10, self, port); expect(invoke(kernel, group, 49, {at, address_pointer, 16}) == -22);
    address6(2, self, port); expect(invoke(kernel, group, 49, {at, address_pointer, 28}) == -97);
    address6(10, mapped, port); expect(invoke(kernel, group, 49, {at, address_pointer, 28}) == -22);
    address6(10, remote, port); expect(invoke(kernel, group, 49, {at, address_pointer, 28}) == -99);
    // Linux admits the RFC 2133 length; the native record is exact.
    address6(10, self, port); expect(invoke(kernel, group, 49, {at, address_pointer, 24}) == (linux ? 0 : -22));
    if (!linux) expect(invoke(kernel, group, 49, {at, address_pointer, 28}) == 0);
    expect(kernel.native_tcp().snapshot(handle).value().local == (net::tcp::Endpoint{self, port}));
    address(link.peer_ip, 7000); expect(invoke(kernel, group, 42, {at, address_pointer, 16}) == -22);
    address6(2, router, 7000); expect(invoke(kernel, group, 42, {at, address_pointer, 28}) == -97);
    address6(10, mapped, 7000); expect(invoke(kernel, group, 42, {at, address_pointer, 28}) == -101);
    address6(10, remote, 7000); expect(invoke(kernel, group, 42, {at, address_pointer, 28}) == -101);
    address6(10, router, 7000); expect(invoke(kernel, group, 42, {at, address_pointer, 28}) == -115);
    // A first SYN waits for neighbour discovery; the router's advert releases it.
    Peer peer; CHECK(peer.passive_open({router, 7000}, 4000 + port, mock::now / 1000000));
    poll6(kernel, &peer);
    std::array<std::byte, 128> advert{};
    auto na = wire::build_neighbor_advertisement(std::span{advert}.subspan(14), router, self, router, true, true, true, link.peer_mac);
    CHECK(na && wire::build_ethernet(advert, link.local_mac, link.peer_mac, wire::ethertype_ipv6, na.value()));
    std::copy_n(advert.begin(), 14 + na.value(), extension.incoming[extension.incoming_head].begin());
    extension.incoming_lengths[extension.incoming_head] = 14 + na.value(); extension.incoming_count = 1;
    poll6(kernel, &peer); poll6(kernel, &peer);
    extension.send_peer(peer, wire::ethertype_ipv6); poll6(kernel, &peer);
    expect(peer.snapshot().state == net::tcp::State::established);
    const std::array<std::byte, 3> data{std::byte{'v'}, std::byte{'6'}, std::byte{'!'}};
    std::copy(data.begin(), data.end(), mock::memory.begin() + 128);
    expect(invoke(kernel, group, 1, {at, payload_pointer, data.size()}) == 3);
    poll6(kernel, &peer);
    std::array<std::byte, 8> received{}; auto read = peer.read(received); CHECK(read); expect(read.value() == 3 && received[1] == std::byte{'6'});
    // Connected IPv6 names: 28 bytes, family 10, scope 1 for link-local.
    for (u32 number : {51u, 52u})
    {
        const auto name = name_call(kernel, group, number, static_cast<i32>(at), 28);
        const auto &ip = number == 51 ? self : router;
        const u16 expected_port = number == 51 ? port : 7000;
        expect(name.result == 0 && name.length == 28 && name.record[0] == 10 && name.record[1] == 0);
        expect(name.record[2] == static_cast<u8>(expected_port >> 8) && name.record[3] == static_cast<u8>(expected_port));
        expect(std::equal(ip.begin(), ip.end(), name.record.begin() + 8) && name.record[24] == 1);
    }
    expect(invoke(kernel, group, 3, {at}) == 0);

    // A listener's accepted peer is a 28-byte sockaddr_in6 with scope 1.
    const auto listener = invoke(kernel, group, 41, {10, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}); expect(listener >= 3);
    const auto server = static_cast<uptr>(listener);
    address6(10, self, static_cast<u16>(port + 1));
    expect(invoke(kernel, group, 49, {server, address_pointer, 28}) == 0);
    expect(invoke(kernel, group, 50, {server, 2}) == 0);
    Peer client; CHECK(client.active_open({router, 7001}, {self, static_cast<u16>(port + 1)}, 9000 + port, mock::now / 1000000));
    extension.send_peer(client, wire::ethertype_ipv6); poll6(kernel, &client);
    extension.send_peer(client, wire::ethertype_ipv6); poll6(kernel, &client);
    expect(client.snapshot().state == net::tcp::State::established);
    constexpr uptr length_pointer = hw::user_begin + 896;
    const u32 capacity = 64; std::memcpy(mock::memory.data() + 896, &capacity, sizeof(capacity));
    std::fill_n(mock::memory.begin() + 768, 64, std::byte{0x5a});
    const auto accepted = invoke(kernel, group, 288, {server, address_pointer, length_pointer, 0}); expect(accepted >= 3);
    u32 reported{}; std::memcpy(&reported, mock::memory.data() + 896, sizeof(reported)); expect(reported == 28);
    auto name = lu::Ipv6SocketAddressCodec<lu::Profile::x86_64>::decode(
        {reinterpret_cast<const u8 *>(mock::memory.data() + 768), 28});
    expect(name && name.value().family == 10 && name.value().port == 7001 && name.value().address == router);
    expect(name.value().scope_id == 1 && name.value().flowinfo == 0 && mock::memory[768 + 28] == std::byte{0x5a});
    expect(kernel.native_tcp().snapshot(stream(kernel, group, static_cast<i32>(accepted))).value().family == net::Family::ipv6);
    expect(invoke(kernel, group, 3, {static_cast<uptr>(accepted)}) == 0);
    expect(invoke(kernel, group, 3, {server}) == 0);
    retire(kernel, group);
}
// A blocking poll over a stream sleeps on the endpoint's wait queue: idle
// passes leave it unevaluated and the peer's data wakes it.
native::PollWait *poll_wait(sched::ProcessId pid)
{
    for (const auto &entry : native::poll_waits)
        if (entry.pid == pid)
            if (auto resolved = native::poll_objects.resolve(entry.object)) return resolved.value();
    return nullptr;
}
void poll_until_data(Kernel &kernel, Group group, i32 fd, Peer &peer, std::span<const std::byte> data)
{
    auto *caller = kernel.scheduler().find_thread(group.pid, group.pid); CHECK(caller);
    constexpr uptr fds = hw::user_begin + 896;
    ok_pollfd entry{fd, OK_POLLIN, 0}; std::memcpy(mock::memory.data() + 896, &entry, sizeof(entry));
    CHECK(kernel.scheduler().activate_thread(group.pid, group.pid)); CHECK(kernel.posix().activate(group.pid));
    hw::UserFrame frame{}; frame.pc = hw::user_begin + 256; frame.sp = hw::user_begin + 4096;
    frame.registers[14] = group.linux ? 7 : OK_SYS_POLL; frame.registers[9] = fds; frame.registers[10] = 1;
    frame.registers[11] = ~uptr{0};
    caller->native_frame = frame; caller->syscall_pending = false;
    mock::entry_trap = {hw::TrapKind::syscall, 256}; native::last_pid = native::last_tid = 0;
    expect(native::step(kernel));
    expect(caller->syscall_pending);
    mock::entry_trap = {hw::TrapKind::timer};
    auto *wait = poll_wait(group.pid); expect(wait && wait->socket_only && !wait->dirty);
    for (usize pass = 0; pass < 3; ++pass)
    {
        expect(native::step(kernel)); expect(caller->syscall_pending && wait->socket_only && !wait->dirty);
    }
    CHECK(peer.write(data)); extension.send_peer(peer); CHECK(kernel.poll_network()); extension.receive_peer(peer);
    expect(native::step(kernel));
    expect(!caller->syscall_pending && static_cast<i64>(caller->native_frame.registers[14]) == 1 && !poll_wait(group.pid));
    std::memcpy(&entry, mock::memory.data() + 896, sizeof(entry)); expect(entry.revents == OK_POLLIN);
}
// sendto/recvfrom on a connected stream are write/read with flags: TCP
// ignores a destination (after copying it) and names no source, so a
// requested source length becomes 0 (Linux __sys_sendto, __sys_recvfrom).
void stream_messages(Kernel &kernel, Group group, i32 fd, Peer &peer)
{
    const auto at = static_cast<uptr>(fd);
    const std::array<std::byte, 3> data{std::byte{'s'}, std::byte{'n'}, std::byte{'d'}};
    std::copy(data.begin(), data.end(), mock::memory.begin() + 128);
    expect(invoke(kernel, group, 44, {at, payload_pointer, 3, 0x100, 0, 0}) == -95); // MSG_WAITALL
    address(link.peer_ip, 9);
    expect(invoke(kernel, group, 44, {at, payload_pointer, 3, 0, address_pointer, 129}) == -22);
    expect(invoke(kernel, group, 44, {at, payload_pointer, 3, OK_MSG_NOSIGNAL | OK_MSG_DONTWAIT, address_pointer, 16}) == 3);
    CHECK(kernel.poll_network()); extension.receive_peer(peer);
    std::array<std::byte, 8> got{}; auto read = peer.read(got); CHECK(read);
    expect(read.value() == 3 && got[0] == std::byte{'s'} && got[2] == std::byte{'d'});
    const auto deliver = [&] {
        CHECK(peer.write(data)); extension.send_peer(peer); CHECK(kernel.poll_network()); extension.receive_peer(peer);
    };
    const auto capacity = [](i32 value) { std::memcpy(mock::memory.data() + 1000, &value, sizeof(value)); };
    const auto length = [] { i32 value{}; std::memcpy(&value, mock::memory.data() + 1000, sizeof(value)); return value; };
    deliver();
    expect(invoke(kernel, group, 45, {at, payload_pointer, 8, 0x100, 0, 0}) == -95);
    capacity(16);
    expect(invoke(kernel, group, 45, {at, payload_pointer, 8, OK_MSG_PEEK, address_pointer, name_length_pointer}) == 3);
    expect(length() == 0);
    capacity(16);
    expect(invoke(kernel, group, 45, {at, payload_pointer, 8, OK_MSG_DONTWAIT, address_pointer, name_length_pointer}) == 3);
    expect(length() == 0 && mock::memory[128] == std::byte{'s'});
    expect(invoke(kernel, group, 45, {at, payload_pointer, 8, 0, 0, 0}) == -11);
    // A negative source length is EINVAL after the bytes were taken.
    deliver(); capacity(-1);
    expect(invoke(kernel, group, 45, {at, payload_pointer, 8, 0, address_pointer, name_length_pointer}) == -22);
    expect(invoke(kernel, group, 45, {at, payload_pointer, 8, 0, 0, 0}) == -11);
}
// The router answers an IPv6 SYN with port unreachable: the executor routes
// the ICMPv6 error to the connection (after ipv6_exercise resolved the router).
void icmp6_refusal(Kernel &kernel, u16 port)
{
    namespace wire = net::wire;
    const auto self = wire::link_local_from_mac(link.local_mac), router = wire::link_local_from_mac(link.peer_mac);
    const auto group = create(kernel, true);
    const auto fd = invoke(kernel, group, 41, {10, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}); expect(fd >= 3);
    const auto at = static_cast<uptr>(fd);
    address6(10, self, port); expect(invoke(kernel, group, 49, {at, address_pointer, 28}) == 0);
    address6(10, router, 7300); expect(invoke(kernel, group, 42, {at, address_pointer, 28}) == -115);
    CHECK(kernel.poll_network());
    bool quoted = false;
    for (; extension.count; extension.head = (extension.head + 1) % extension.outgoing.size(), --extension.count)
    {
        const auto &frame = extension.outgoing[extension.head];
        auto ethernet = wire::ethernet({frame.data(), extension.lengths[extension.head]}); CHECK(ethernet);
        if (ethernet.value().type != wire::ethertype_ipv6) continue;
        auto ip = wire::ipv6(ethernet.value().payload);
        if (quoted || !ip || ip.value().protocol != wire::protocol_tcp) continue;
        // Only this connection's SYN; earlier connections may still send.
        auto segment = wire::tcp<wire::Ipv6Family>(ip.value());
        if (!segment || segment.value().fields.source_port != port) continue;
        auto &bytes = extension.incoming[extension.incoming_head];
        auto built = wire::build_icmp_error<wire::Ipv6Family>(std::span{bytes}.subspan(14), router, self, 0,
            wire::icmpv6::unreachable, wire::icmpv6::port_unreachable, 0, ethernet.value().payload);
        CHECK(built && wire::build_ethernet(bytes, link.local_mac, link.peer_mac, wire::ethertype_ipv6, built.value()));
        extension.incoming_lengths[extension.incoming_head] = built.value() + 14; extension.incoming_count = 1;
        quoted = true;
    }
    expect(quoted);
    const auto errors = kernel.native_ethernet_executor().state().icmp_errors;
    CHECK(kernel.poll_network()); extension.head = extension.count = 0;
    expect(kernel.native_ethernet_executor().state().icmp_errors == errors + 1);
    expect(invoke(kernel, group, 0, {at, payload_pointer, 8}) == -111);
    expect(invoke(kernel, group, 0, {at, payload_pointer, 8}) == 0);
    expect(invoke(kernel, group, 3, {at}) == 0);
    retire(kernel, group);
}
void exercise(Kernel &kernel, bool linux, u16 port)
{
    auto group = create(kernel, linux);
    expect(invoke(kernel, group, 41, {3, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}) == -97);
    auto fd = invoke(kernel, group, 41, {2, OK_SOCK_STREAM | OK_SOCK_NONBLOCK | OK_SOCK_CLOEXEC, 6}); expect(fd >= 3);
    const auto handle = stream(kernel, group, static_cast<i32>(fd)); expect(kernel.native_tcp().valid(handle));
    expect(kernel.native_udp().active_sockets() == 0);
    address(link.local_ip, port); expect(invoke(kernel, group, 49, {static_cast<uptr>(fd), address_pointer, 16}) == 0);
    address(link.peer_ip, 7000);
    expect(invoke(kernel, group, 42, {static_cast<uptr>(fd), address_pointer, 16}) == -115);
    expect(invoke(kernel, group, 42, {static_cast<uptr>(fd), address_pointer, 16}) == -114);
    expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), payload_pointer, 8}) == -11);
    Peer peer; CHECK(peer.passive_open({link.peer_ip, 7000}, 4000 + port, mock::now / 1000000));
    CHECK(kernel.poll_network()); extension.receive_peer(peer); extension.send_peer(peer);
    CHECK(kernel.poll_network()); extension.receive_peer(peer);
    expect(peer.snapshot().state == net::tcp::State::established);
    const std::array<std::byte, 5> english{std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};
    std::copy(english.begin(), english.end(), mock::memory.begin() + 128);
    expect(invoke(kernel, group, 1, {static_cast<uptr>(fd), payload_pointer, english.size()}) == 5);
    CHECK(kernel.poll_network()); extension.receive_peer(peer);
    std::array<std::byte, 16> received{}; auto read = peer.read(received); CHECK(read); expect(read.value() == 5);
    expect(std::equal(english.begin(), english.end(), received.begin()));
    poll_until_data(kernel, group, static_cast<i32>(fd), peer, english);
    const auto endpoint = *kernel.native_tcp().snapshot(handle).value().endpoint;
    expect(kernel.native_tcp().endpoints().state(endpoint).value().transport.receive_buffered == 5);
    mock::write_forbidden = payload_pointer;
    expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), payload_pointer, english.size()}) == -14);
    expect(kernel.native_tcp().endpoints().state(endpoint).value().transport.receive_buffered == 5);
    mock::write_forbidden = 0;
    expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), payload_pointer, english.size()}) == 5);
    expect(std::equal(english.begin(), english.end(), mock::memory.begin() + 128));
    expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), 0, 0}) == 0);
    stream_messages(kernel, group, static_cast<i32>(fd), peer);
    auto duplicate = invoke(kernel, group, 32, {static_cast<uptr>(fd)}); expect(duplicate >= 3);
    expect(invoke(kernel, group, 3, {static_cast<uptr>(fd)}) == 0); expect(kernel.native_tcp().valid(handle));
    CHECK(peer.shutdown_write()); extension.send_peer(peer); CHECK(kernel.poll_network()); extension.receive_peer(peer);
    expect(invoke(kernel, group, 0, {static_cast<uptr>(duplicate), payload_pointer, 8}) == 0);
    expect(invoke(kernel, group, 48, {static_cast<uptr>(duplicate), OK_SHUT_WR}) == 0);
    CHECK(kernel.poll_network()); extension.receive_peer(peer); extension.send_peer(peer); CHECK(kernel.poll_network());
    expect(invoke(kernel, group, 3, {static_cast<uptr>(duplicate)}) == 0); expect(!kernel.native_tcp().valid(handle));
    retire(kernel, group);
}
void listener_errors(Kernel &kernel, bool linux, u16 port)
{
    const auto group = create(kernel, linux);
    const auto fd = invoke(kernel, group, 41, {2, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}); expect(fd >= 3);
    address(link.local_ip, port);
    expect(invoke(kernel, group, 49, {static_cast<uptr>(fd), address_pointer, 16}) == 0);
    expect(invoke(kernel, group, 50, {static_cast<uptr>(fd), 2}) == 0);
    expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), payload_pointer, 8}) == -107);
    expect(invoke(kernel, group, 1, {static_cast<uptr>(fd), payload_pointer, 8}) == -107);
    expect(invoke(kernel, group, 48, {static_cast<uptr>(fd), OK_SHUT_WR}) == -107);
    expect(invoke(kernel, group, 42, {static_cast<uptr>(fd), address_pointer, 16}) == -22);
    // Accept is implemented; an empty nonblocking listener would block.
    for (u32 accept : {43u, 288u})
        expect(invoke(kernel, group, accept, {static_cast<uptr>(fd), 0, 0}) == -11);
    expect(poll_now(kernel, group, static_cast<i32>(fd), OK_POLLIN | OK_POLLOUT) == 0);
    // getsockname reports the bound name; a short capacity copies a prefix
    // and still reports 16; a negative one is EINVAL. A listener has no peer.
    auto name = name_call(kernel, group, 51, static_cast<i32>(fd), 16);
    expect(name.result == 0 && name.length == 16 && name.record[0] == 2 && name.record[1] == 0);
    expect(name.record[2] == static_cast<u8>(port >> 8) && name.record[3] == static_cast<u8>(port));
    expect(std::equal(link.local_ip.begin(), link.local_ip.end(), name.record.begin() + 4) && name.record[16] == 0xa5);
    name = name_call(kernel, group, 51, static_cast<i32>(fd), 4);
    expect(name.result == 0 && name.length == 16 && name.record[3] == static_cast<u8>(port) && name.record[4] == 0xa5);
    name = name_call(kernel, group, 51, static_cast<i32>(fd), -1);
    expect(name.result == -22 && name.record[0] == 0xa5);
    expect(name_call(kernel, group, 52, static_cast<i32>(fd), 16).result == -107);
    expect(invoke(kernel, group, 51, {static_cast<uptr>(fd), 0, name_length_pointer}) == -14);
    expect(invoke(kernel, group, 55, {static_cast<uptr>(fd), 0, 0}) == -95);
    expect(invoke(kernel, group, 3, {static_cast<uptr>(fd)}) == 0);
    retire(kernel, group);
}
// The gateway answers the SYN with an ICMP unreachable: the handshake fails
// with the errno Linux tcp_v4_err reports for it.
// `report` picks what reports the failure: a read, SO_ERROR or a second connect.
enum class Report { read, option, connect };
void icmp_refusal(Kernel &kernel, bool linux, u16 port, u8 code, i64 expected, Report report)
{
    const auto group = create(kernel, linux);
    const auto fd = invoke(kernel, group, 41, {2, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}); expect(fd >= 3);
    constexpr i16 both = OK_POLLIN | OK_POLLOUT;
    // Unconnected is TCP_CLOSE: writable and hung up (Linux tcp_poll).
    expect(poll_now(kernel, group, static_cast<i32>(fd), both) == (OK_POLLOUT | OK_POLLHUP));
    address(link.local_ip, port); expect(invoke(kernel, group, 49, {static_cast<uptr>(fd), address_pointer, 16}) == 0);
    address(link.peer_ip, 7100);
    expect(invoke(kernel, group, 42, {static_cast<uptr>(fd), address_pointer, 16}) == -115);
    expect(poll_now(kernel, group, static_cast<i32>(fd), both) == 0);
    CHECK(kernel.poll_network());
    bool quoted = false;
    for (; extension.count; extension.head = (extension.head + 1) % extension.outgoing.size(), --extension.count)
    {
        const auto &frame = extension.outgoing[extension.head];
        auto ethernet = net::wire::ethernet({frame.data(), extension.lengths[extension.head]}); CHECK(ethernet);
        auto ip = net::wire::ipv4(ethernet.value().payload);
        if (quoted || !ip || ip.value().protocol != net::wire::protocol_tcp) continue;
        auto &bytes = extension.incoming[extension.incoming_head];
        auto built = net::wire::build_icmp_error<net::wire::Ipv4Family>(std::span{bytes}.subspan(14), link.peer_ip,
            link.local_ip, 3, net::wire::icmpv4::unreachable, code, 0, ethernet.value().payload);
        CHECK(built);
        for (usize at = 0; at < 6; ++at)
        { bytes[at] = std::byte{link.local_mac[at]}; bytes[at + 6] = std::byte{link.peer_mac[at]}; }
        net::wire::be16(bytes, 12, 0x0800);
        extension.incoming_lengths[extension.incoming_head] = built.value() + 14; extension.incoming_count = 1;
        quoted = true;
    }
    expect(quoted);
    const auto errors = kernel.native_ethernet_executor().state().icmp_errors;
    CHECK(kernel.poll_network()); extension.head = extension.count = 0;
    expect(kernel.native_ethernet_executor().state().icmp_errors == errors + 1);
    // A refused connect is shut down both ways and failed.
    expect(poll_now(kernel, group, static_cast<i32>(fd), both) == (OK_POLLIN | OK_POLLOUT | OK_POLLERR | OK_POLLHUP));
    const auto f = static_cast<i32>(fd);
    // The failure is reported once (Linux sock_error).
    if (report == Report::read) expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), payload_pointer, 8}) == expected);
    if (report == Report::connect)
    {
        address(link.peer_ip, 7100);
        expect(invoke(kernel, group, 42, {static_cast<uptr>(fd), address_pointer, 16}) == expected);
    }
    if (report == Report::option)
    {
        // A negative length is EINVAL before the error is taken (sk_getsockopt).
        expect(option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_ERROR, -1).result == -22);
        const auto taken = option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_ERROR);
        expect(taken.result == 0 && taken.length == 4 && taken.value == static_cast<u32>(-expected));
    }
    auto none = option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_ERROR);
    expect(none.result == 0 && none.length == 4 && none.value == 0);
    // Then the closed connection reads EOF, writes EPIPE and has no POLLERR.
    expect(poll_now(kernel, group, f, both) == (OK_POLLIN | OK_POLLOUT | OK_POLLHUP));
    expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), payload_pointer, 8}) == 0);
    expect(invoke(kernel, group, 1, {static_cast<uptr>(fd), payload_pointer, 8}) == -32);
    expect(name_call(kernel, group, 52, f, 16).result == -107);
    // SO_TYPE; a short capacity copies a prefix and reports it; negative is EINVAL.
    auto type = option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_TYPE);
    expect(type.result == 0 && type.length == 4 && type.value == OK_SOCK_STREAM);
    type = option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_TYPE, 1);
    expect(type.result == 0 && type.length == 1 && (type.value & 0xff) == OK_SOCK_STREAM && (type.value >> 8) == 0xa5a5a5);
    expect(option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_TYPE, -1).result == -22);
    expect(option_call(kernel, group, f, 6, 1).result == -95);
    expect(invoke(kernel, group, 3, {static_cast<uptr>(fd)}) == 0);
    retire(kernel, group);
}
// A connected UDP socket whose peer refused a datagram: the next sendto
// reports ECONNREFUSED once (both personalities), and SO_ERROR takes it too.
void udp_refusal(Kernel &kernel, bool linux)
{
    const auto group = create(kernel, linux);
    // Nonblocking: a blocking Linux UDP send needs its captured continuation.
    const auto fd = invoke(kernel, group, 41, {2, OK_SOCK_DGRAM | OK_SOCK_NONBLOCK, 0}); expect(fd >= 3);
    const auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    const auto *description = kernel.scheduler().open_description(process->fd_table().entry(static_cast<i32>(fd))->description);
    CHECK(description && description->native_udp_handle);
    const u32 udp = description->native_udp_handle;
    CHECK(kernel.native_udp().connect(udp, link.peer_ip, 7200));
    const auto binding = kernel.native_udp().binding(udp); CHECK(binding);
    const net::wire::IcmpErrorClass refused{net::wire::IcmpErrorKind::refused, true, 0};
    const auto refuse = [&] {
        auto taken = kernel.native_udp().icmp_error(binding.value().address, binding.value().port, link.peer_ip, 7200, refused);
        expect(taken && taken.value());
    };
    const std::array<std::byte, 3> bytes{std::byte{'u'}, std::byte{'d'}, std::byte{'p'}};
    std::copy(bytes.begin(), bytes.end(), mock::memory.begin() + 128);
    const auto send = [&] {
        address(link.peer_ip, 7200);
        return invoke(kernel, group, 44, {static_cast<uptr>(fd), payload_pointer, bytes.size(), 0, address_pointer, 16});
    };
    refuse();
    expect(send() == -111);
    expect(send() == 3);
    extension.head = extension.count = 0;
    const auto f = static_cast<i32>(fd);
    refuse();
    expect(option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_ERROR, -1).result == -22);
    auto taken = option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_ERROR);
    expect(taken.result == 0 && taken.length == 4 && taken.value == 111);
    taken = option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_ERROR);
    expect(taken.result == 0 && taken.value == 0);
    const auto type = option_call(kernel, group, f, OK_SOL_SOCKET, OK_SO_TYPE);
    expect(type.result == 0 && type.length == 4 && type.value == OK_SOCK_DGRAM);
    expect(option_call(kernel, group, f, 0, 1).result == -95);
    expect(invoke(kernel, group, 3, {static_cast<uptr>(fd)}) == 0);
    retire(kernel, group);
}
// connect on an unbound stream binds it first (inet_autobind): the source
// address the interface selects and a port of the ephemeral range.
void autobind_connect(Kernel &kernel, bool linux)
{
    const auto group = create(kernel, linux);
    const auto fd = invoke(kernel, group, 41, {2, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}); expect(fd >= 3);
    const auto f = static_cast<i32>(fd);
    auto name = name_call(kernel, group, 51, f, 16);
    expect(name.result == 0 && name.record[2] == 0 && name.record[3] == 0);
    address(link.peer_ip, 7400);
    expect(invoke(kernel, group, 42, {static_cast<uptr>(fd), address_pointer, 16}) == -115);
    name = name_call(kernel, group, 51, f, 16);
    const u16 port = static_cast<u16>((name.record[2] << 8) | name.record[3]);
    expect(name.result == 0 && name.length == 16 && port >= 49152);
    expect(std::equal(link.local_ip.begin(), link.local_ip.end(), name.record.begin() + 4));
    // A second unbound stream takes another port.
    const auto other = invoke(kernel, group, 41, {2, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}); expect(other >= 3);
    address(link.peer_ip, 7400); // name_call wrote its record over the address.
    expect(invoke(kernel, group, 42, {static_cast<uptr>(other), address_pointer, 16}) == -115);
    const auto second = name_call(kernel, group, 51, static_cast<i32>(other), 16);
    expect(second.result == 0 && static_cast<u16>((second.record[2] << 8) | second.record[3]) != port);
    CHECK(kernel.poll_network()); extension.head = extension.count = 0;
    expect(invoke(kernel, group, 3, {static_cast<uptr>(fd)}) == 0);
    expect(invoke(kernel, group, 3, {static_cast<uptr>(other)}) == 0);
    retire(kernel, group);
}
void original_guard_closes(Kernel &kernel)
{
    const auto group = create(kernel, false);
    const auto fd = invoke(kernel, group, 41, {2, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}); expect(fd >= 3);
    const auto duplicate = invoke(kernel, group, 32, {static_cast<uptr>(fd)}); expect(duplicate >= 3);
    auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    const auto description = process->fd_table().entry(static_cast<i32>(fd))->description;
    const auto handle = stream(kernel, group, static_cast<i32>(fd));
    Status cleanup{};
    native::TcpOriginalDescription original{kernel.scheduler(), kernel.native_tcp(), cleanup};
    auto selected = original.acquire(*process, static_cast<i32>(fd)); CHECK(selected);
    expect(selected.value() == native::TcpDescriptionKind::retained_tcp && original.live());
    expect(kernel.scheduler().open_description(description)->references == 3);
    CHECK(kernel.scheduler().close_descriptor(process->resources(), static_cast<i32>(duplicate)));
    expect(original.live() && kernel.scheduler().open_description(description)->references == 2);
    CHECK(kernel.scheduler().close_descriptor(process->resources(), static_cast<i32>(fd)));
    expect(!original.live() && kernel.native_tcp().valid(handle));
    // Reinstalling the same held OFD at the original number cannot resurrect
    // the descriptor edge that the scheduler's close notification revoked.
    CHECK(kernel.scheduler().install_descriptor(process->resources(), static_cast<i32>(fd), description, false));
    expect(!original.live() && kernel.scheduler().open_description(description)->references == 2);
    CHECK(original.release()); CHECK(original.release());
    expect(cleanup.ok() && kernel.native_tcp().valid(handle));
    expect(kernel.scheduler().open_description(description)->references == 1);
    expect(invoke(kernel, group, 3, {static_cast<uptr>(fd)}) == 0);
    expect(!kernel.native_tcp().valid(handle) && !kernel.scheduler().open_description(description));
    retire(kernel, group);
}
}
int main()
{
    static Kernel kernel; KernelConfig config{}; config.extension = &extension; CHECK(kernel.boot(config));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    kernel.posix().set_entropy_source(&entropy);
    original_guard_closes(kernel);
    ipv6_exercise(kernel, false, 56001); ipv6_exercise(kernel, true, 56011);
    icmp6_refusal(kernel, 56021);
    listener_errors(kernel, false, 55001); listener_errors(kernel, true, 55002);
    exercise(kernel, false, 54001); exercise(kernel, true, 54002);
    icmp_refusal(kernel, false, 53001, net::wire::icmpv4::port_unreachable, -111, Report::read);
    icmp_refusal(kernel, true, 53002, net::wire::icmpv4::port_unreachable, -111, Report::option);
    icmp_refusal(kernel, false, 53005, net::wire::icmpv4::port_unreachable, -111, Report::option);
    icmp_refusal(kernel, true, 53003, 10, -113, Report::connect);
    icmp_refusal(kernel, true, 53004, 6, -101, Report::read);
    udp_refusal(kernel, false); udp_refusal(kernel, true);
    autobind_connect(kernel, false); autobind_connect(kernel, true);
    const auto before = kernel.native_udp_executor().state(); expect(before.active && !before.failed);
    extension.failed = true; expect(!kernel.poll_network());
    const auto after = kernel.native_udp_executor().state(); expect(after.failed && after.readiness_sequence > before.readiness_sequence);
    expect(kernel.native_tcp().endpoints().pools().retired);
    std::printf("TCP_IMMEDIATE_RUNTIME_HOSTED_PASS checks=%zu real_runtime=1 real_origin=1 guest_execution=0 socket_execution=0 nic_execution=0\n", checks);
}
