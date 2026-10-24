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
    void send_peer(Peer &peer)
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
            net::wire::be16(bytes, 12, 0x0800); incoming_lengths[slot] = packet.length + 14; ++incoming_count;
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
void exercise(Kernel &kernel, bool linux, u16 port)
{
    auto group = create(kernel, linux);
    expect(invoke(kernel, group, 41, {10, OK_SOCK_STREAM | OK_SOCK_NONBLOCK, 6}) == -97);
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
    CHECK(peer.write(english)); extension.send_peer(peer); CHECK(kernel.poll_network()); extension.receive_peer(peer);
    const auto endpoint = *kernel.native_tcp().snapshot(handle).value().endpoint;
    expect(kernel.native_tcp().endpoints().state(endpoint).value().transport.receive_buffered == 5);
    mock::write_forbidden = payload_pointer;
    expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), payload_pointer, english.size()}) == -14);
    expect(kernel.native_tcp().endpoints().state(endpoint).value().transport.receive_buffered == 5);
    mock::write_forbidden = 0;
    expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), payload_pointer, english.size()}) == 5);
    expect(std::equal(english.begin(), english.end(), mock::memory.begin() + 128));
    expect(invoke(kernel, group, 0, {static_cast<uptr>(fd), 0, 0}) == 0);
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
    for (u32 deferred : {43u, 51u, 52u, 55u, 288u})
        expect(invoke(kernel, group, deferred, {static_cast<uptr>(fd), 0, 0}) == -95);
    expect(invoke(kernel, group, 3, {static_cast<uptr>(fd)}) == 0);
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
    listener_errors(kernel, false, 55001); listener_errors(kernel, true, 55002);
    exercise(kernel, false, 54001); exercise(kernel, true, 54002);
    const auto before = kernel.native_udp_executor().state(); expect(before.active && !before.failed);
    extension.failed = true; expect(!kernel.poll_network());
    const auto after = kernel.native_udp_executor().state(); expect(after.failed && after.readiness_sequence > before.readiness_sequence);
    expect(kernel.native_tcp().endpoints().pools().retired);
    std::printf("TCP_IMMEDIATE_RUNTIME_HOSTED_PASS checks=%zu real_runtime=1 real_origin=1 guest_execution=0 socket_execution=0 nic_execution=0\n", checks);
}
