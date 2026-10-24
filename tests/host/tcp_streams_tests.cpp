#include <ok/net/tcp_stream_checked.hpp>
#include <ok/net/native_tcp_ethernet.hpp>
#include "../../src/user/tcp_immediate_operation.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>
using namespace ok;
using namespace ok::net;
using wire::tcp_syn;
using wire::tcp_ack;
namespace
{
usize checks{};
u64 next_identity{10000};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { std::fprintf(stderr, "TCP_STREAM_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #value); std::abort(); } } while(false)
constexpr tcp::Endpoint local{{192, 0, 2, 2}, 32001}, peer{{192, 0, 2, 1}, 8080};
constexpr NativeUdpLink link{{2, 1, 2, 3, 4, 5}, {2, 6, 7, 8, 9, 10}, local.address, peer.address};
struct Iss { Result<u32> operator()(tcp::Endpoint, tcp::Endpoint) { return u32{700}; } };
struct Packet
{
    std::array<std::byte, 1514> bytes{}; usize length{};
    wire::Bytes view() const { return {bytes.data(), length}; }
};
Packet packet(tcp::Endpoint from, tcp::Endpoint to, u32 seq, u32 ack, u8 flags, wire::Bytes payload = {})
{
    Packet out; auto built = wire::build_tcp(out.bytes, from.address, to.address, 23, 64,
        {from.port, to.port, seq, ack, flags, 4096}, {}, payload);
    CHECK(built); out.length = built.value(); return out;
}
Packet ethernet(const Packet &ip, bool wrong_peer = false)
{
    Packet out; out.length = ip.length + 14;
    for (usize byte = 0; byte < 6; ++byte)
    { out.bytes[byte] = std::byte{link.local_mac[byte]}; out.bytes[byte + 6] = std::byte{link.peer_mac[byte]}; }
    if (wrong_peer) out.bytes[7] ^= std::byte{1};
    wire::be16(out.bytes, 12, 0x0800);
    for (usize byte = 0; byte < ip.length; ++byte) out.bytes[byte + 14] = ip.bytes[byte];
    return out;
}
Packet arp_request()
{
    Packet out; out.length = 42;
    for (usize byte = 0; byte < 6; ++byte)
    { out.bytes[byte] = std::byte{0xff}; out.bytes[byte + 6] = std::byte{link.peer_mac[byte]}; out.bytes[22 + byte] = std::byte{link.peer_mac[byte]}; }
    wire::be16(out.bytes, 12, 0x0806); wire::be16(out.bytes, 14, 1); wire::be16(out.bytes, 16, 0x0800);
    out.bytes[18] = std::byte{6}; out.bytes[19] = std::byte{4}; wire::be16(out.bytes, 20, 1);
    for (usize byte = 0; byte < 4; ++byte)
    { out.bytes[28 + byte] = std::byte{link.peer_ip[byte]}; out.bytes[38 + byte] = std::byte{link.local_ip[byte]}; }
    return out;
}
struct Copy
{
    std::array<std::byte, 4096> bytes{}; bool preflight_fail{}, read_fail{}, write_fail{};
    usize preflights{}, reads{}, writes{};
    Status preflight(u64 address, usize length, bool)
    {
        ++preflights;
        return !preflight_fail && address >= 4096 && address - 4096 <= bytes.size() &&
            length <= bytes.size() - static_cast<usize>(address - 4096) ? Status{} : Status::fault("copy preflight failure");
    }
    Status read(u64 address, std::span<std::byte> output)
    {
        ++reads; if (read_fail) return Status::fault("copy read failure");
        const usize at = static_cast<usize>(address - 4096);
        for (usize i = 0; i < output.size(); ++i) output[i] = bytes[at + i];
        return {};
    }
    Status write(u64 address, wire::Bytes input)
    {
        ++writes; if (write_fail) return Status::fault("copy write failure");
        const usize at = static_cast<usize>(address - 4096);
        for (usize i = 0; i < input.size(); ++i) bytes[at + i] = input[i];
        return {};
    }
};
struct Fixture
{
    sched::Scheduler scheduler{}; NativeTcpStreamService service{tcp::TableIdentity{next_identity++}};
    TcpStreamWaits waits{scheduler, service}; TcpStreamOperations operations{scheduler, service};
    NativeUdpService udp{}; NativeTcpEthernetExecutor executor{}; Iss iss{};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(4096, 32768)};
    u64 pid{};
    Fixture()
    {
        CHECK(scheduler.set_native_tcp_service(service)); CHECK(waits.status()); CHECK(udp.configure(link));
        auto created = scheduler.create_process("tcp-owner", context); CHECK(created); pid = created.value();
        CHECK(scheduler.set_runnable(pid)); CHECK(scheduler.activate(pid));
    }
    ~Fixture()
    { CHECK(waits.shutdown()); CHECK(scheduler.drain_all_resources()); CHECK(service.endpoints().retire()); }
    sched::ProcessControlBlock &process() { return *scheduler.find(pid); }
    TcpStreamHandle stream(i32 fd)
    { auto *slot = process().fd_table().entry(fd); CHECK(slot && slot->used); return scheduler.open_description(slot->description)->native_tcp_handle; }
    u16 description(i32 fd) { return process().fd_table().entry(fd)->description; }
    Packet take(u64 now)
    {
        Packet out; auto prepared = service.endpoints().prepare(out.bytes, now); CHECK(prepared && prepared.value());
        out.length = prepared.value()->length; CHECK(service.endpoints().complete(prepared.value()->ticket, true, now)); return out;
    }
    void ingress(const Packet &bytes, u64 now)
    { CHECK(service.endpoints().ingest(bytes.view(), now, iss)); }
    i32 connected()
    {
        auto opened = operations.open(pid); CHECK(opened); const i32 fd = opened.value();
        CHECK(operations.bind(pid, fd, local)); CHECK(operations.connect(pid, fd, peer, 100, 0)); take(0);
        ingress(packet(peer, local, 200, 101, tcp_syn | tcp_ack), 1); take(1);
        CHECK(service.readiness(stream(fd)).value().connect_complete); return fd;
    }
    sched::WaitTicket wait(i32 fd, u8 interests = tcp::Readiness::read_interest, std::optional<u64> deadline = {})
    {
        CHECK(scheduler.activate(pid)); auto ticket = waits.arm(pid, fd, interests, deadline); CHECK(ticket); return ticket.value();
    }
};
std::unique_ptr<Fixture> fixture() { return std::make_unique<Fixture>(); }

void descriptor_lifecycle()
{
    static_assert(!std::is_copy_constructible_v<NativeTcpStreamService> && !std::is_move_constructible_v<NativeTcpStreamService>);
    static_assert(!std::is_copy_constructible_v<TcpStreamOperations> && !std::is_move_constructible_v<TcpStreamWaits>);
    auto owner = fixture(); auto &f = *owner;
    auto opened = f.operations.open(f.pid); CHECK(opened); auto h = f.stream(opened.value()); const auto id = f.description(opened.value());
    CHECK(f.scheduler.open_description(id)->references == 1 && f.service.active_streams() == 1);
    CHECK(!f.scheduler.allocate_description(*f.scheduler.open_description(id)));
    // A write-only UNIX provider field cannot accompany canonical TCP ownership.
    CHECK(user::native::canonical_tcp_fd(f.scheduler, f.process(), opened.value()));
    const auto descriptions_before = f.scheduler.open_description_count();
    auto mixed = *f.scheduler.open_description(id);
    mixed.unix_write_pipe = 0;
    const auto rejected = f.scheduler.allocate_description(mixed);
    CHECK(!rejected && rejected.status().code() == StatusCode::invalid_argument);
    CHECK(f.scheduler.open_description_count() == descriptions_before && f.service.valid(h));
    auto *description = f.scheduler.open_description(id);
    description->unix_write_pipe = 0;
    CHECK(!user::native::canonical_tcp_fd(f.scheduler, f.process(), opened.value()));
    description->unix_write_pipe = sched::invalid_description;
    CHECK(user::native::canonical_tcp_fd(f.scheduler, f.process(), opened.value()) && f.service.valid(h));
    CHECK(f.scheduler.install_descriptor(f.process().resources(), 4, id));
    CHECK(f.scheduler.open_description(id)->references == 2);
    auto child = f.scheduler.fork_process(f.pid); CHECK(child);
    CHECK(f.scheduler.open_description(id)->references == 4 && f.service.active_streams() == 1);
    CHECK(f.scheduler.close_descriptor(f.process().resources(), opened.value()));
    CHECK(f.scheduler.close_descriptor(f.process().resources(), 4));
    CHECK(f.service.valid(h)); CHECK(f.scheduler.exit_process(child.value(), 0));
    CHECK(!f.service.valid(h) && f.service.active_streams() == 0 && !f.scheduler.open_description(id));
    auto next = f.operations.open(f.pid); CHECK(next); auto reused = f.stream(next.value());
    CHECK(reused.slot == h.slot && reused.generation > h.generation && !f.service.close(h));
    NativeTcpStreamService replacement{tcp::TableIdentity{next_identity++}};
    CHECK(!f.scheduler.set_native_tcp_service(replacement));
    CHECK(f.scheduler.close_descriptor(f.process().resources(), next.value()));
    CHECK(f.scheduler.set_native_tcp_service(replacement)); CHECK(!replacement.valid(reused));
    CHECK(!f.operations.open(f.pid)); CHECK(f.scheduler.set_native_tcp_service(f.service));
    for (usize iteration = 0; iteration < 64; ++iteration)
    {
        auto a = f.operations.open(f.pid); CHECK(a); const auto old = f.stream(a.value());
        CHECK(f.scheduler.close_descriptor(f.process().resources(), a.value()));
        CHECK(!f.service.valid(old) && !f.service.snapshot(old));
    }
}
void capacity_and_publication()
{
    auto scheduler_owner = std::make_unique<sched::Scheduler>(); auto &scheduler = *scheduler_owner;
    auto service_owner = std::make_unique<NativeTcpStreamService>(tcp::TableIdentity{900}, TcpStreamCapacity{2, 1}); auto &service = *service_owner;
    CHECK(scheduler.set_native_tcp_service(service)); TcpStreamOperations operations{scheduler, service};
    auto process = scheduler.create_process("capacity", arch::configured_arch_operations().make_kernel_context(4096, 32768)); CHECK(process);
    for (usize iteration = 0; iteration < 2; ++iteration)
    {
        auto fd = operations.open(process.value()); CHECK(fd);
        CHECK(!operations.open(process.value()));
        CHECK(scheduler.close_descriptor(scheduler.find(process.value())->resources(), fd.value()));
    }
    CHECK(!operations.open(process.value()) && service.active_streams() == 0);
    CHECK(scheduler.open_description_count() == 3); CHECK(scheduler.drain_all_resources());
    auto owner = fixture(); auto &f = *owner;
    for (i32 fd = 3; fd < static_cast<i32>(sched::max_process_fds); ++fd)
        CHECK(f.scheduler.install_descriptor(f.process().resources(), fd, 1));
    const auto before = f.service.active_streams();
    auto failed = f.operations.open(f.pid); CHECK(!failed && failed.status().code() == StatusCode::process_file_limit);
    CHECK(f.service.active_streams() == before);
    CHECK(f.scheduler.close_descriptor(f.process().resources(), 3));
    for (usize i = f.scheduler.open_description_count(); i < sched::max_open_descriptions; ++i)
    { auto reserved = f.scheduler.allocate_description({}); CHECK(reserved); }
    failed = f.operations.open(f.pid); CHECK(!failed && failed.status().code() == StatusCode::system_file_limit);
    CHECK(f.service.active_streams() == before);
    for (u16 id = 3; id < sched::max_open_descriptions; ++id)
        if (auto *open = f.scheduler.open_description(id); open && !open->references) *open = {};
}
void waiter_copy_and_close()
{
    auto owner = fixture(); auto &f = *owner; const auto fd = f.connected(); const auto stream = f.stream(fd); const auto id = f.description(fd);
    CHECK(f.scheduler.install_descriptor(f.process().resources(), 4, id));
    auto ticket = f.wait(fd); CHECK(f.scheduler.open_description(id)->references == 3);
    CHECK(f.scheduler.close_descriptor(f.process().resources(), 4));
    CHECK(!f.waits.observe(ticket) && f.scheduler.open_description(id)->references == 2);
    auto forged = ticket; ++forged.generation; CHECK(!f.waits.observe(forged) && !f.waits.cancel(forged));
    const std::array payload{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    f.ingress(packet(peer, local, 201, 101, tcp_ack, payload), 2); CHECK(f.waits.poll());
    CHECK(f.waits.observe(ticket).value().completion == sched::WaitCompletion::ready);
    Copy copy; copy.preflight_fail = true;
    CHECK(!f.operations.read(f.pid, fd, 4096, 3, copy)); CHECK(f.service.readiness(stream).value().readable_bytes == 3);
    CHECK(f.waits.observe(ticket).value().owns_description);
    copy.preflight_fail = false; copy.write_fail = true;
    CHECK(!f.operations.read(f.pid, fd, 4096, 3, copy)); CHECK(f.service.readiness(stream).value().readable_bytes == 3);
    CHECK(f.waits.observe(ticket).value().owns_description);
    copy.write_fail = false; auto read = f.operations.read(f.pid, fd, 4096, 3, copy); CHECK(read && read.value() == 3);
    CHECK(copy.bytes[0] == payload[0] && copy.bytes[2] == payload[2]); CHECK(f.waits.finish(ticket));
    CHECK(!f.waits.finish(ticket) && f.scheduler.open_description(id)->references == 1);
    ticket = f.wait(fd); CHECK(f.scheduler.close_descriptor(f.process().resources(), fd));
    CHECK(f.waits.observe(ticket).value().completion == sched::WaitCompletion::cancelled);
    CHECK(!f.waits.observe(ticket).value().owns_description && f.service.active_streams() == 0);
    CHECK(!f.scheduler.open_description(id)); CHECK(f.waits.finish(ticket));
    const auto calls = copy.preflights; CHECK(!f.operations.read(f.pid, fd, 4096, 1, copy)); CHECK(copy.preflights == calls);
}
void waiter_completion_controls()
{
    auto owner = fixture(); auto &f = *owner; const auto fd = f.connected(); const auto id = f.description(fd);
    auto ticket = f.wait(fd, tcp::Readiness::read_interest, 20);
    CHECK(f.service.endpoints().tick(20)); CHECK(f.waits.poll());
    CHECK(f.waits.observe(ticket).value().completion == sched::WaitCompletion::timeout);
    CHECK(f.scheduler.open_description(id)->references == 1); CHECK(f.waits.finish(ticket));
    ticket = f.wait(fd); CHECK(f.waits.cancel(ticket, sched::WaitCompletion::interrupted));
    CHECK(f.waits.observe(ticket).value().completion == sched::WaitCompletion::interrupted);
    CHECK(f.scheduler.open_description(id)->references == 1); CHECK(f.waits.finish(ticket));
    ticket = f.wait(fd); CHECK(f.scheduler.complete_wait(ticket, sched::WaitCompletion::interrupted)); CHECK(f.waits.poll());
    CHECK(f.waits.observe(ticket).value().completion == sched::WaitCompletion::interrupted); CHECK(f.waits.finish(ticket));
    ticket = f.wait(fd); f.scheduler.open_description(id)->flags |= TcpStreamOperations::nonblock; CHECK(f.waits.poll());
    CHECK(f.waits.observe(ticket).value().completion == sched::WaitCompletion::ready); CHECK(f.waits.finish(ticket));
    CHECK(f.scheduler.activate(f.pid)); CHECK(!f.waits.arm(f.pid, fd, tcp::Readiness::read_interest));
    f.scheduler.open_description(id)->flags &= ~TcpStreamOperations::nonblock;
    ticket = f.wait(fd); CHECK(f.scheduler.replace_image(f.pid, f.context, {}));
    CHECK(!f.waits.observe(ticket) && f.waits.records() == 0 && f.scheduler.open_description(id)->references == 1);
    ticket = f.wait(fd); CHECK(f.scheduler.exit_process(f.pid, 0));
    CHECK(!f.waits.observe(ticket) && f.service.active_streams() == 0 && f.waits.records() == 0);
    CHECK(!f.operations.open(f.pid));
}
void listener_retirement_and_callback_close()
{
    auto owner = fixture(); auto &f = *owner;
    auto opened = f.operations.open(f.pid); CHECK(opened);
    CHECK(f.operations.bind(f.pid, opened.value(), local)); CHECK(f.operations.listen(f.pid, opened.value(), 2));
    const auto old = f.stream(opened.value());
    struct ClosingIss
    {
        Fixture &fixture; i32 fd;
        Result<u32> operator()(tcp::Endpoint, tcp::Endpoint)
        {
            CHECK(fixture.scheduler.close_descriptor(fixture.process().resources(), fd));
            CHECK(fixture.service.pending_closes() == 1); return u32{400};
        }
    } closing{f, opened.value()};
    auto syn = packet(peer, local, 300, 0, tcp_syn);
    CHECK(f.service.endpoints().ingest(syn.view(), 1, closing));
    CHECK(!f.service.valid(old) && f.service.active_streams() == 1);
    NativeTcpStreamService replacement{tcp::TableIdentity{next_identity++}};
    CHECK(!f.scheduler.set_native_tcp_service(replacement));
    CHECK(f.service.drain_closes()); CHECK(f.service.active_streams() == 0);
    CHECK(f.service.endpoints().pools().unaccepted == 0 && f.service.endpoints().pools().listeners == 0);
    opened = f.operations.open(f.pid); CHECK(opened);
    CHECK(f.operations.bind(f.pid, opened.value(), local)); CHECK(f.operations.listen(f.pid, opened.value(), 1));
    auto ticket = f.wait(opened.value(), tcp::Readiness::accept_interest);
    CHECK(f.service.endpoints().retire()); CHECK(f.waits.poll()); CHECK(f.waits.observe(ticket));
    CHECK(f.scheduler.close_descriptor(f.process().resources(), opened.value()));
    CHECK(f.waits.observe(ticket).value().completion == sched::WaitCompletion::cancelled);
    CHECK(f.waits.finish(ticket)); CHECK(f.service.active_streams() == 0);
}
void service_replacement_preserves_protocol()
{
    NativeTcpStreamService replacement{tcp::TableIdentity{next_identity++}};
    auto owner = fixture(); auto &f = *owner; const auto fd = f.connected();
    const auto handle = f.stream(fd); const auto endpoint = *f.service.snapshot(handle).value().endpoint;
    CHECK(f.scheduler.close_descriptor(f.process().resources(), fd));
    CHECK(f.service.active_streams() == 0 && f.service.endpoints().pools().retiring == 1);
    CHECK(!f.scheduler.set_native_tcp_service(replacement));
    CHECK(f.service.endpoints().pools().retiring == 1 && !f.service.endpoints().state(endpoint));
    CHECK(f.service.endpoints().retire()); CHECK(f.scheduler.set_native_tcp_service(replacement));
    CHECK(!replacement.valid(handle));
}
template <abi::linux_user::Profile Profile> void linux_profile()
{
    using Constants = abi::linux_user::UdpConstants<Profile>;
    auto owner = fixture(); auto &f = *owner; LinuxTcpStreamAdapter<Profile> adapter{f.operations}; Copy copy;
    auto opened = adapter.socket(f.pid, 2, Constants::sock_stream | Constants::sock_nonblock | Constants::sock_cloexec, 6);
    CHECK(opened); const auto fd = opened.value(); auto handle = f.stream(fd);
    CHECK((f.scheduler.open_description(f.description(fd))->flags & TcpStreamOperations::nonblock) != 0);
    CHECK(f.process().fd_table().entry(fd)->close_on_exec);
    CHECK(!adapter.socket(f.pid, 2, Constants::sock_dgram, 6)); CHECK(!adapter.socket(f.pid, 2, Constants::sock_stream, 17));
    copy.bytes[0] = abi::linux_user::Codec<Profile>::profile.big_endian ? std::byte{0} : std::byte{2};
    copy.bytes[1] = abi::linux_user::Codec<Profile>::profile.big_endian ? std::byte{2} : std::byte{0};
    copy.bytes[2] = std::byte{0x7d}; copy.bytes[3] = std::byte{0x01};
    for (usize i = 0; i < 4; ++i) copy.bytes[4 + i] = std::byte{local.address[i]};
    copy.read_fail = true; CHECK(!adapter.bind(f.pid, fd, 4096, 16, copy)); CHECK(f.service.snapshot(handle).value().local.port == 0);
    copy.read_fail = false; CHECK(!adapter.bind(f.pid, fd, 4096, 15, copy));
    CHECK(adapter.bind(f.pid, fd, 4096, 16, copy)); CHECK(f.service.snapshot(handle).value().local == local);
    CHECK(!adapter.bind(f.pid, fd, abi::linux_user::Codec<Profile>::word_mask - 7, 16, copy));
    CHECK(!adapter.read(f.pid, fd, 4096, abi::linux_user::Codec<Profile>::word_mask, copy));
    CHECK(adapter.error_number(StatusCode::unsupported) == Constants::errno_number(abi::linux_user::UdpErrno::e_opnotsupp));
    CHECK(adapter.error_number(StatusCode::would_block) == 11); CHECK(adapter.error_number(StatusCode::bad_descriptor) == 9);
    CHECK(adapter.error_number(StatusCode::system_file_limit) == 23); CHECK(!adapter.error_number(StatusCode::interrupted));
    const std::array<std::pair<StatusCode, abi::linux_user::UdpErrno>, 13> errors{{
        {StatusCode::invalid_argument, abi::linux_user::UdpErrno::e_inval},
        {StatusCode::no_memory, abi::linux_user::UdpErrno::e_nomem},
        {StatusCode::already_exists, abi::linux_user::UdpErrno::e_addrinuse},
        {StatusCode::denied, abi::linux_user::UdpErrno::e_acces},
        {StatusCode::permission, abi::linux_user::UdpErrno::e_acces},
        {StatusCode::unsupported, abi::linux_user::UdpErrno::e_opnotsupp},
        {StatusCode::overflow, abi::linux_user::UdpErrno::e_overflow},
        {StatusCode::would_block, abi::linux_user::UdpErrno::e_again},
        {StatusCode::fault, abi::linux_user::UdpErrno::e_fault},
        {StatusCode::bad_descriptor, abi::linux_user::UdpErrno::e_badf},
        {StatusCode::io_error, abi::linux_user::UdpErrno::e_io},
        {StatusCode::process_file_limit, abi::linux_user::UdpErrno::e_mfile},
        {StatusCode::system_file_limit, abi::linux_user::UdpErrno::e_nfile}}};
    for (const auto &error : errors) CHECK(adapter.error_number(error.first) == Constants::errno_number(error.second));
    abi::linux_user::RegisterFile frame{};
    CHECK(adapter.encode_result(frame, Status::unsupported()));
    CHECK(abi::linux_user::Codec<Profile>::decode_return(frame).value().error == *Constants::errno_number(abi::linux_user::UdpErrno::e_opnotsupp));
    const auto &profile = abi::linux_user::Codec<Profile>::profile;
    const auto error = *Constants::errno_number(abi::linux_user::UdpErrno::e_opnotsupp);
    if (profile.error_convention == abi::linux_user::ErrorConvention::negative)
        CHECK(frame.integer[profile.return_register] == ((~u64{0} - error + 1) & abi::linux_user::Codec<Profile>::word_mask));
    else
        CHECK(frame.integer[profile.return_register] == error);
    const auto unchanged = frame.integer; CHECK(!adapter.encode_result(frame, Status::interrupted())); CHECK(frame.integer == unchanged);
}
struct Device
{
    std::array<Packet, 8> incoming{}, outgoing{}; usize first{}, count{}, sent{}, reaps{}, receives{};
    usize credit{8}, completed{}; bool block_tx{}, fail_tx{}, fail_rx{}, fail_reap{};
    void push(Packet packet) { CHECK(count < incoming.size()); incoming[(first + count++) % incoming.size()] = packet; }
    Result<NativeUdpTransmitState> reap(usize)
    { ++reaps; return fail_reap ? Result<NativeUdpTransmitState>{Status::io_error()} : Result<NativeUdpTransmitState>{NativeUdpTransmitState{completed, credit}}; }
    Result<usize> receive(std::span<std::byte> output)
    {
        ++receives; if (fail_rx) return Status::io_error(); if (!count) return Status::would_block();
        const auto &packet = incoming[first]; CHECK(packet.length <= output.size());
        for (usize i = 0; i < packet.length; ++i) output[i] = packet.bytes[i];
        const auto length = packet.length; first = (first + 1) % incoming.size(); --count; return length;
    }
    Status try_transmit(wire::Bytes frame)
    {
        if (block_tx) return Status::would_block();
        if (fail_tx) return Status::io_error();
        CHECK(sent < outgoing.size()); auto &packet = outgoing[sent++]; packet.length = frame.size();
        for (usize i = 0; i < frame.size(); ++i) packet.bytes[i] = frame[i];
        return {};
    }
};
void ethernet_integration()
{
    auto owner = fixture(); auto &f = *owner; Device device;
    auto fd = f.operations.open(f.pid); CHECK(fd); CHECK(f.operations.bind(f.pid, fd.value(), local));
    CHECK(f.operations.connect(f.pid, fd.value(), peer, 100, 0));
    auto ticket = f.wait(fd.value(), tcp::Readiness::connect_interest);
    CHECK(f.executor.poll(f.service, f.udp, f.waits, device, f.iss, 0)); CHECK(device.sent == 1);
    auto eth = wire::ethernet(device.outgoing[0].view()); CHECK(eth && eth.value().source == link.local_mac);
    auto ip = wire::ipv4(eth.value().payload); CHECK(ip && wire::tcp(ip.value()).value().fields.flags == tcp_syn);
    device.push(ethernet(packet(peer, local, 200, 101, tcp_syn | tcp_ack), true));
    device.push(ethernet(packet(peer, local, 200, 101, tcp_syn | tcp_ack)));
    auto udp_socket = f.udp.open(); CHECK(udp_socket); CHECK(f.udp.bind(udp_socket.value(), local.address, 42000));
    NativeUdpService peer_udp; CHECK(peer_udp.configure({link.peer_mac, link.local_mac, link.peer_ip, link.local_ip}));
    auto peer_socket = peer_udp.open(); CHECK(peer_socket); CHECK(peer_udp.bind(peer_socket.value(), peer.address, 43000));
    const std::array payload{std::byte{'x'}, std::byte{'y'}}; Packet udp;
    auto datagram = peer_udp.build_datagram(peer_socket.value(), local.address, 42000, payload, udp.bytes); CHECK(datagram);
    udp.length = datagram.value(); device.push(udp);
    CHECK(f.executor.poll(f.service, f.udp, f.waits, device, f.iss, 1));
    CHECK(f.waits.observe(ticket).value().completion == sched::WaitCompletion::ready); CHECK(f.waits.finish(ticket));
    CHECK(f.executor.state().tcp_received == 1 && f.executor.state().udp_received == 1 && f.executor.state().dropped == 1);
    CHECK(f.udp.peek(udp_socket.value()).value().payload.size() == 2); CHECK(f.udp.consume(udp_socket.value()));
    ticket = f.wait(fd.value()); device.push(ethernet(packet(peer, local, 201, 101, tcp_ack, payload)));
    CHECK(f.executor.poll(f.service, f.udp, f.waits, device, f.iss, 2)); CHECK(f.waits.observe(ticket));
    Copy copy; auto read = f.operations.read(f.pid, fd.value(), 4096, 2, copy); CHECK(read && read.value() == 2); CHECK(f.waits.finish(ticket));
    ticket = f.wait(fd.value()); copy.bytes[0] = std::byte{'z'}; CHECK(f.operations.write(f.pid, fd.value(), 4096, 1, copy));
    device.fail_tx = true; CHECK(!f.executor.poll(f.service, f.udp, f.waits, device, f.iss, 3));
    CHECK(f.executor.state().retired && !f.service.endpoints().pools().prepared);
    CHECK(f.waits.observe(ticket).value().completion == sched::WaitCompletion::ready); CHECK(f.waits.finish(ticket));
    CHECK(f.scheduler.close_descriptor(f.process().resources(), fd.value())); CHECK(f.udp.close(udp_socket.value()));
}
void ethernet_failure_controls()
{
    for (usize control = 0; control < 4; ++control)
    {
        auto owner = fixture(); auto &f = *owner; Device device;
        if (control == 0) device.completed = 9;
        if (control == 1) device.credit = 9;
        if (control == 2) device.fail_rx = true;
        if (control == 3) device.fail_reap = true;
        CHECK(!f.executor.poll(f.service, f.udp, f.waits, device, f.iss, 0)); CHECK(f.executor.state().retired);
    }
    auto owner = fixture(); auto &f = *owner; Device device; device.credit = 0;
    device.push(arp_request()); device.push(arp_request()); device.push(arp_request());
    CHECK(f.executor.poll(f.service, f.udp, f.waits, device, f.iss, 10));
    CHECK(f.executor.state().controls == 2 && f.executor.state().dropped == 1);
    const auto calls = device.reaps; CHECK(!f.executor.poll(f.service, f.udp, f.waits, device, f.iss, 9));
    CHECK(device.reaps == calls && !f.executor.state().retired);
    device.credit = 8; device.block_tx = true; CHECK(f.executor.poll(f.service, f.udp, f.waits, device, f.iss, 10));
    CHECK(f.executor.state().controls == 2 && device.sent == 0);
    device.block_tx = false; CHECK(f.executor.poll(f.service, f.udp, f.waits, device, f.iss, 11));
    CHECK(device.sent == 2 && f.executor.state().controls == 0);
    for (usize i = 0; i < 2; ++i)
    { auto eth = wire::ethernet(device.outgoing[i].view()); CHECK(eth && wire::arp(eth.value().payload).value().operation == 2); }
}
}
int main()
{
    descriptor_lifecycle(); capacity_and_publication(); waiter_copy_and_close(); waiter_completion_controls(); listener_retirement_and_callback_close(); service_replacement_preserves_protocol();
    linux_profile<abi::linux_user::Profile::i386>(); linux_profile<abi::linux_user::Profile::x86_64>();
    linux_profile<abi::linux_user::Profile::aarch64>(); linux_profile<abi::linux_user::Profile::arm32>();
    linux_profile<abi::linux_user::Profile::rv64>(); linux_profile<abi::linux_user::Profile::rv32>();
    linux_profile<abi::linux_user::Profile::loongarch64>(); linux_profile<abi::linux_user::Profile::mips>();
    linux_profile<abi::linux_user::Profile::mips64>(); linux_profile<abi::linux_user::Profile::ppc>();
    ethernet_integration(); ethernet_failure_controls();
    std::printf("TCP_STREAMS_HOSTED_PASS checks=%zu real_scheduler=1 guest_execution=0 socket_execution=0 nic_execution=0\n", checks);
}
