// Actual x86_64 runtime dispatch, captured access and canonical POSIX/OFD code.
// Trap, mappings and link frames are explicit hosted models; no guest/NIC runs.
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_udp_receive_model.hpp"
#include <source_location>
static_assert(lu::configured_native_profile() == lu::Profile::x86_64);

namespace
{
using C = lu::UdpConstants<lu::Profile::x86_64>;
using E = lu::UdpErrno;
constexpr uptr payload_address = hw::user_begin + 128;
constexpr uptr source_address = hw::user_begin + 512;
constexpr uptr source_length = hw::user_begin + 640;
constexpr uptr input_address = hw::user_begin + 768;
constexpr net::NativeUdpLink network_fixture{{2, 0x11, 0x22, 0x33, 0x44, 0x55},
    {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}, {192, 0, 2, 2}, {192, 0, 2, 1}};
constexpr std::array<std::byte, 3> bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
usize checks{}, invocations{};
void expect(bool value, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "LINUX_UDP_RUNTIME_FAIL line=%u checks=%zu\n", where.line(), checks);
        std::abort();
    }
}
i64 error(E value) { return -static_cast<i64>(*C::errno_number(value)); }
struct Group { sched::ProcessId pid{}; i32 fd{-1}; u32 endpoint{}; u16 port{51101}; };
net::NativeUdpService peer;
u32 sender{};

// Boot-installed extension models only the actual runtime device boundary.
// It never replaces enter_owned or changes capture counters/history.
class LinkExtension final : public KernelExtension
{
  public:
    Result<net::NativeUdpLink> network_link(Kernel &) override
    { ++link_queries; return network_fixture; }
    Status network_try_transmit(Kernel &, std::span<const std::byte> input) override
    {
        ++submissions;
        expect(input.size() <= transmitted.size());
        std::copy(input.begin(), input.end(), transmitted.begin());
        size = input.size();
        return submission_status;
    }
    usize link_queries{}, submissions{}, size{};
    Status submission_status{};
    std::array<std::byte, net::NativeUdpService::frame_capacity> transmitted{};
};
LinkExtension extension;

Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("linux-udp-runtime-host",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 4096})); CHECK(made);
    auto *process = kernel.scheduler().find(made.value());
    process->resources().personality = sched::UserPersonality::linux;
    process->resources().native_space = {0x100000 + made.value() * 4096, static_cast<u32>(made.value() % 16), 1};
    auto &thread = process->threads()[0]; CHECK(native::prepare_native_thread(thread));
    thread.native_active = true;
    CHECK(kernel.scheduler().set_runnable(made.value()));
    CHECK(kernel.scheduler().activate_thread(made.value(), made.value()));
    CHECK(kernel.posix().activate(made.value()));
    native::runtime_kernel = &kernel; native::started = true;
    return {made.value()};
}
sched::ThreadControlBlock &thread(Kernel &kernel, Group group)
{ return *kernel.scheduler().find_thread(group.pid, group.pid); }
i64 invoke(Kernel &kernel, Group group, lu::UdpCall call, std::array<uptr, 6> arguments)
{
    auto &caller = thread(kernel, group);
    CHECK(kernel.scheduler().activate_thread(group.pid, group.pid)); CHECK(kernel.posix().activate(group.pid));
    hw::UserFrame frame{}; frame.pc = hw::user_begin + 256; frame.sp = hw::user_begin + 4096;
    frame.registers[14] = *C::syscall_number(call);
    constexpr std::array<usize, 6> slots{9, 10, 11, 5, 7, 6};
    for (usize at = 0; at < slots.size(); ++at) frame.registers[slots[at]] = arguments[at];
    caller.native_frame = frame; caller.syscall_pending = false;
    mock::entry_trap = {hw::TrapKind::syscall, 256};
    expect(mock::owned_hook == nullptr);
    native::last_pid = native::last_tid = 0;
    const auto entries = mock::entries;
    expect(native::step(kernel)); ++invocations;
    expect(mock::entries == entries + 1);
    expect(!caller.syscall_pending && caller.native_active);
    expect(native::runtime_status().ok());
    auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    auto old = native::LinuxUdpImmediateAccess<lu::Profile::x86_64>::acquire(native::syscall_origins, *process, caller);
    expect(!old); // Completion consumed image authority, including the frame origin.
    return static_cast<i64>(caller.native_frame.registers[14]);
}
void length(u32 value) { std::memcpy(mock::memory.data() + 640, &value, sizeof(value)); }
u32 length() { u32 value{}; std::memcpy(&value, mock::memory.data() + 640, sizeof(value)); return value; }
void address(u16 port, net::wire::Ipv4 ip = {})
{
    std::array<u8, 16> value{}; CHECK(lu::UdpWire<lu::Profile::x86_64>::encode_address(value, {2, port, ip}));
    std::memcpy(mock::memory.data() + 768, value.data(), value.size());
}
void packet(Kernel &kernel, Group group)
{
    std::array<std::byte, net::NativeUdpService::frame_capacity> frame{}, reply{};
    const auto built = peer.build_datagram(sender, network_fixture.local_ip, group.port, bytes, frame); CHECK(built);
    const auto ingested = kernel.native_udp().ingest({frame.data(), built.value()}, reply); CHECK(ingested);
    expect(ingested.value().queued_handle == group.endpoint);
}

void socket_and_bind(Kernel &kernel, Group &group)
{
    const auto before = kernel.native_udp().active_sockets();
    const auto plain = invoke(kernel, group, lu::UdpCall::socket, {2, C::sock_dgram, 17}); expect(plain >= 3);
    const auto *plain_process = kernel.scheduler().find(group.pid); CHECK(plain_process);
    const auto *plain_slot = plain_process->fd_table().entry(static_cast<i32>(plain)); CHECK(plain_slot);
    const auto *plain_open = kernel.scheduler().open_description(plain_slot->description); CHECK(plain_open);
    expect(!(plain_open->flags & posix::o_NONBLOCK) && !plain_slot->close_on_exec);
    CHECK(kernel.posix().activate(group.pid)); CHECK(kernel.posix().close(static_cast<i32>(plain)));
    expect(kernel.native_udp().active_sockets() == before);
    expect(invoke(kernel, group, lu::UdpCall::socket, {10, C::sock_dgram | C::sock_nonblock, 17}) == error(E::e_afnosupport));
    const auto fd = invoke(kernel, group, lu::UdpCall::socket,
        {2, C::sock_dgram | C::sock_nonblock | C::sock_cloexec, 17}); expect(fd >= 3);
    group.fd = static_cast<i32>(fd);
    const auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    const auto *slot = process->fd_table().entry(group.fd); expect(slot && slot->used && slot->close_on_exec);
    const auto *open = kernel.scheduler().open_description(slot->description); CHECK(open);
    group.endpoint = open->native_udp_handle;
    expect(group.endpoint && open->references == 1 && (open->flags & posix::o_NONBLOCK));
    expect(extension.link_queries == 1 && kernel.native_udp().configured());
    address(group.port);
    expect(invoke(kernel, group, lu::UdpCall::bind, {static_cast<uptr>(group.fd), input_address, 16}) == 0);
    expect(kernel.native_udp().binding(group.endpoint).value().port == group.port);
    expect(invoke(kernel, group, lu::UdpCall::bind, {static_cast<uptr>(group.fd), input_address, 16}) == error(E::e_inval));
    const auto copies = mock::copies;
    expect(invoke(kernel, group, lu::UdpCall::bind, {static_cast<uptr>(group.fd), input_address, ~uptr{}}) == error(E::e_inval));
    expect(mock::copies == copies);
    const auto second = invoke(kernel, group, lu::UdpCall::socket, {2, C::sock_dgram | C::sock_nonblock, 0}); expect(second >= 3);
    expect(invoke(kernel, group, lu::UdpCall::bind, {static_cast<uptr>(second), input_address, 16}) == error(E::e_addrinuse));
    CHECK(kernel.posix().activate(group.pid));
    CHECK(kernel.posix().close(static_cast<i32>(second)));
}
template <lu::Profile P> void target_completion_models()
{
    using Constants = lu::UdpConstants<P>;
    for (usize index = 0; index < static_cast<usize>(E::count); ++index)
    {
        const auto number = Constants::errno_number(static_cast<E>(index)); expect(number.has_value());
        hw::UserFrame frame{}; frame.registers[7] = 17;
        expect(native::encode_linux_udp_target_result<P>(frame, -static_cast<i64>(*number)).ok());
        const auto result = lu::NativeBridge<P>::decode_return(frame); expect(result.ok());
        expect(result.value().error == *number && result.value().value == 0);
    }
    for (i64 value : {i64{0}, i64{1}, i64{16}, i64{1472}})
    {
        hw::UserFrame frame{}; frame.registers[7] = 17;
        expect(native::encode_linux_udp_target_result<P>(frame, value).ok());
        const auto result = lu::NativeBridge<P>::decode_return(frame); expect(result.ok());
        expect(result.value().error == 0 && result.value().value == static_cast<u64>(value));
    }
    hw::UserFrame frame{}; frame.pc = 73; frame.registers[7] = 17;
    const auto before = frame;
    expect(!native::encode_linux_udp_target_result<P>(frame, -4096));
    expect(frame.registers == before.registers && frame.fp_state == before.fp_state && frame.pc == before.pc);
    constexpr bool mips = P == lu::Profile::mips || P == lu::Profile::mips64;
    expect(native::linux_udp_delay_slot_excluded<P>({hw::TrapKind::syscall, 8, 0x80000020U}) == mips);
    expect(!native::linux_udp_delay_slot_excluded<P>({hw::TrapKind::syscall, 8, 0x20}));
    expect(!native::linux_udp_delay_slot_excluded<P>({hw::TrapKind::syscall, 8, 0x80000024U}));
    expect(!native::linux_udp_delay_slot_excluded<P>({hw::TrapKind::syscall, 0, 0x80000020U}));
    expect(!native::linux_udp_delay_slot_excluded<P>({hw::TrapKind::timer, 8, 0x80000020U}));
}
void send_effects(Kernel &kernel, Group group)
{
    const auto fd = static_cast<uptr>(group.fd);
    const auto submits = extension.submissions;
    const auto copies = mock::copies;
    expect(invoke(kernel, group, lu::UdpCall::sendto, {fd, payload_address, 3}) == error(E::e_destaddrreq));
    expect(extension.submissions == submits && mock::copies == copies);
    address(52001, network_fixture.peer_ip);
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 1, input_address, 16}) == error(E::e_opnotsupp));
    expect(extension.submissions == submits && mock::copies == copies + 1);
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, ~uptr{}}) == error(E::e_inval));
    expect(extension.submissions == submits && mock::copies == copies + 1);
    address(52001, {192, 0, 2, 99});
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == error(E::e_netunreach));
    expect(extension.submissions == submits && mock::copies == copies + 2);
    address(52001, network_fixture.peer_ip);
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, net::NativeUdpService::datagram_capacity + 1, 0, input_address, 16}) == error(E::e_msgsize));
    expect(extension.submissions == submits && mock::copies == copies + 3);
    mock::forbidden = payload_address;
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == error(E::e_fault));
    expect(extension.submissions == submits && mock::copies == copies + 5);
    mock::forbidden = 0;
    std::copy(bytes.begin(), bytes.end(), mock::memory.begin() + 128);
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == 3);
    expect(extension.submissions == submits + 1);
    std::array<std::byte, net::NativeUdpService::frame_capacity> reply{};
    CHECK(peer.bind(sender, {}, 52001));
    const auto received = peer.ingest({extension.transmitted.data(), extension.size}, reply); CHECK(received);
    expect(received.value().queued_handle == sender);
    const auto datagram = peer.peek(sender); CHECK(datagram);
    expect(datagram.value().payload.size() == bytes.size() && datagram.value().source_port == group.port);
    expect(std::memcmp(bytes.data(), datagram.value().payload.data(), bytes.size()) == 0);
    CHECK(peer.consume(sender));
    const auto outputs = mock::outputs;
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, 0, 0, 0, input_address, 16}) == 0);
    expect(extension.submissions == submits + 2 && mock::outputs == outputs);
    const auto empty = peer.ingest({extension.transmitted.data(), extension.size}, reply); CHECK(empty);
    expect(empty.value().queued_handle == sender && peer.peek(sender).value().payload.empty());
    CHECK(peer.consume(sender));
    extension.submission_status = Status::would_block("synthetic device credit exhausted");
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == error(E::e_again));
    extension.submission_status = Status::not_initialized("synthetic link down");
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == error(E::e_netdown));
    extension.submission_status = Status::success();
    const auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    const auto *slot = process->fd_table().entry(group.fd); CHECK(slot);
    expect(kernel.scheduler().open_description(slot->description)->references == 1);
}
void receive_effects(Kernel &kernel, Group group)
{
    const auto fd = static_cast<uptr>(group.fd);
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {fd, payload_address, 3}) == error(E::e_again));
    packet(kernel, group); mock::write_forbidden = payload_address;
    const auto outputs = mock::outputs;
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {fd, payload_address, 3}) == error(E::e_fault));
    expect(mock::outputs == outputs + 1);
    expect(kernel.native_udp().peek(group.endpoint).status().code() == StatusCode::would_block);
    packet(kernel, group);
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {fd, payload_address, 3, 2}) == error(E::e_fault));
    expect(kernel.native_udp().peek(group.endpoint).ok()); mock::write_forbidden = 0;
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {fd, payload_address, 2, 32}) == 3);
    expect(mock::memory[128] == std::byte{'a'} && mock::memory[129] == std::byte{'b'});
    expect(!kernel.native_udp().peek(group.endpoint));
    packet(kernel, group); length(0xffffffffU);
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {fd, payload_address, 3, 0, source_address, source_length}) == error(E::e_inval));
    expect(mock::memory[130] == std::byte{'c'} && length() == 0xffffffffU);
    expect(!kernel.native_udp().peek(group.endpoint));
    packet(kernel, group); length(0);
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {fd, payload_address, 3, 0, source_address, source_length}) == 3);
    expect(length() == 16); expect(!kernel.native_udp().peek(group.endpoint));
    packet(kernel, group);
    const auto old_outputs = mock::outputs;
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {fd, 0, 0, 0, 0, ~uptr{}}) == 0);
    expect(mock::outputs == old_outputs); expect(!kernel.native_udp().peek(group.endpoint));
    const auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    const auto *slot = process->fd_table().entry(group.fd); CHECK(slot);
    expect(kernel.scheduler().open_description(slot->description)->references == 1);
}
void range_and_descriptor_order(Kernel &kernel, Group group)
{
    const auto copies = mock::copies, outputs = mock::outputs;
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {31, hw::user_begin + mock::memory.size() + 128, 3}) == error(E::e_badf));
    expect(mock::copies == copies && mock::outputs == outputs);
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {31, hw::user_end + 1, 3}) == error(E::e_fault));
    expect(mock::copies == copies && mock::outputs == outputs);
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {0, payload_address, 3}) == error(E::e_notsock));
    packet(kernel, group);
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {static_cast<uptr>(group.fd), payload_address, 3, 1}) == error(E::e_opnotsupp));
    expect(kernel.native_udp().peek(group.endpoint).ok()); CHECK(kernel.native_udp().consume(group.endpoint));
}
// Additional calls use the inherited actual runtime, public OFDs and service.
Group unbound_socket(Kernel &kernel, Group owner)
{
    Group group{owner.pid};
    const auto fd = invoke(kernel, group, lu::UdpCall::socket,
        {C::af_inet, C::sock_dgram | C::sock_nonblock, 17});
    expect(fd >= 3);
    group.fd = static_cast<i32>(fd);
    const auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    const auto *slot = process->fd_table().entry(group.fd); CHECK(slot);
    const auto *description = kernel.scheduler().open_description(slot->description); CHECK(description);
    group.endpoint = description->native_udp_handle;
    const auto binding = kernel.native_udp().binding(group.endpoint); CHECK(binding);
    expect(!binding.value().bound && binding.value().port == 0);
    expect(description->references == 1 && (description->flags & posix::o_NONBLOCK));
    return group;
}
u16 check_binding(Kernel &kernel, Group group, bool bound, u16 previous = 0,
                  net::wire::Ipv4 ip = {})
{
    const auto binding = kernel.native_udp().binding(group.endpoint); CHECK(binding);
    expect(binding.value().bound == bound);
    if (bound)
    {
        expect(binding.value().port >= 49152 && binding.value().address == ip);
        if (previous) expect(binding.value().port == previous);
    }
    else expect(binding.value().port == 0 && binding.value().address == net::wire::Ipv4{});
    const auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    const auto *slot = process->fd_table().entry(group.fd); CHECK(slot);
    const auto *description = kernel.scheduler().open_description(slot->description); CHECK(description);
    expect(description->native_udp_handle == group.endpoint && description->references == 1);
    return binding.value().port;
}
void close_socket(Kernel &kernel, Group group)
{
    const auto before = kernel.native_udp().active_sockets();
    CHECK(kernel.posix().activate(group.pid)); CHECK(kernel.posix().close(group.fd));
    expect(kernel.native_udp().active_sockets() + 1 == before);
    expect(kernel.native_udp().binding(group.endpoint).status().code() == StatusCode::bad_descriptor);
}
void fresh_send_error(Kernel &kernel, Group owner, std::array<uptr, 6> arguments,
                      E result, bool reserved, usize copied)
{
    auto group = unbound_socket(kernel, owner);
    arguments[0] = static_cast<uptr>(group.fd);
    const auto inputs = mock::copies, outputs = mock::outputs, submissions = extension.submissions;
    expect(invoke(kernel, group, lu::UdpCall::sendto, arguments) == error(result));
    expect(mock::copies == inputs + copied && mock::outputs == outputs);
    expect(extension.submissions == submissions);
    check_binding(kernel, group, reserved);
    close_socket(kernel, group);
}
void bind_zero_runtime(Kernel &kernel, Group owner)
{
    auto any_group = unbound_socket(kernel, owner);
    const auto inputs = mock::copies;
    address(0);
    expect(invoke(kernel, any_group, lu::UdpCall::bind,
        {static_cast<uptr>(any_group.fd), input_address, 16}) == 0);
    expect(mock::copies == inputs + 1);
    const auto any_port = check_binding(kernel, any_group, true);
    expect(invoke(kernel, any_group, lu::UdpCall::bind,
        {static_cast<uptr>(any_group.fd), input_address, 16}) == error(E::e_inval));
    check_binding(kernel, any_group, true, any_port);

    auto local_group = unbound_socket(kernel, owner);
    address(0, {192, 0, 2, 99});
    expect(invoke(kernel, local_group, lu::UdpCall::bind,
        {static_cast<uptr>(local_group.fd), input_address, 16}) == error(E::e_addrnotavail));
    check_binding(kernel, local_group, false);
    const auto signed_inputs = mock::copies;
    address(0, network_fixture.local_ip);
    expect(invoke(kernel, local_group, lu::UdpCall::bind,
        {static_cast<uptr>(local_group.fd), input_address, ~uptr{}}) == error(E::e_inval));
    expect(mock::copies == signed_inputs);
    check_binding(kernel, local_group, false);
    expect(invoke(kernel, local_group, lu::UdpCall::bind,
        {static_cast<uptr>(local_group.fd), input_address, 16}) == 0);
    const auto local_port = check_binding(kernel, local_group, true, 0, network_fixture.local_ip);
    expect(local_port != any_port);

    auto conflict = unbound_socket(kernel, owner);
    address(any_port);
    expect(invoke(kernel, conflict, lu::UdpCall::bind,
        {static_cast<uptr>(conflict.fd), input_address, 16}) == error(E::e_addrinuse));
    check_binding(kernel, conflict, false);
    close_socket(kernel, conflict);
    close_socket(kernel, local_group);
    close_socket(kernel, any_group);
}
void autobind_order_runtime(Kernel &kernel, Group owner)
{
    const auto inputs = mock::copies, submissions = extension.submissions;
    expect(invoke(kernel, owner, lu::UdpCall::sendto,
        {31, payload_address, 3, 0, input_address, ~uptr{}}) == error(E::e_badf));
    expect(invoke(kernel, owner, lu::UdpCall::sendto,
        {31, hw::user_end + 1, 3, 0, input_address, ~uptr{}}) == error(E::e_fault));
    expect(mock::copies == inputs && extension.submissions == submissions);

    address(52001, network_fixture.peer_ip);
    fresh_send_error(kernel, owner, {0, hw::user_end + 1, 3, 0, input_address, 16}, E::e_fault, false, 0);
    fresh_send_error(kernel, owner, {0, payload_address, 3, 0, input_address, ~uptr{}}, E::e_inval, false, 0);
    fresh_send_error(kernel, owner, {0, payload_address, 3, 0, input_address, 129}, E::e_inval, false, 0);
    mock::forbidden = input_address;
    fresh_send_error(kernel, owner, {0, payload_address, 65536, 1, input_address, 16}, E::e_fault, false, 1);
    mock::forbidden = 0;

    fresh_send_error(kernel, owner, {0, payload_address, 3, 0, 0, ~uptr{}}, E::e_destaddrreq, true, 0);
    fresh_send_error(kernel, owner, {0, payload_address, 65536, 1, 0, ~uptr{}}, E::e_msgsize, true, 0);
    fresh_send_error(kernel, owner, {0, payload_address, 3, 0, input_address, 0}, E::e_inval, true, 0);
    fresh_send_error(kernel, owner, {0, payload_address, 3, 0, input_address, 8}, E::e_inval, true, 1);
    fresh_send_error(kernel, owner, {0, payload_address, 3, 1, input_address, 16}, E::e_opnotsupp, true, 1);
    fresh_send_error(kernel, owner, {0, payload_address, 65536, 1, input_address, 16}, E::e_msgsize, true, 1);
    mock::memory[768] = std::byte{10};
    fresh_send_error(kernel, owner, {0, payload_address, 3, 0, input_address, 16}, E::e_afnosupport, true, 1);
    fresh_send_error(kernel, owner, {0, payload_address, 65536, 0, input_address, 16}, E::e_msgsize, true, 1);
    address(0, network_fixture.peer_ip);
    fresh_send_error(kernel, owner, {0, payload_address, 3, 0, input_address, 16}, E::e_inval, true, 1);
    address(52001, {192, 0, 2, 99});
    fresh_send_error(kernel, owner, {0, payload_address, 1473, 0, input_address, 16}, E::e_netunreach, true, 1);
    address(52001, network_fixture.peer_ip);
    fresh_send_error(kernel, owner, {0, payload_address, 1473, 0, input_address, 16}, E::e_msgsize, true, 1);
    mock::forbidden = payload_address;
    fresh_send_error(kernel, owner, {0, payload_address, 3, 0, input_address, 128}, E::e_fault, true, 2);
    mock::forbidden = 0;
}
void autobind_persistence_runtime(Kernel &kernel, Group owner)
{
    auto group = unbound_socket(kernel, owner);
    const auto fd = static_cast<uptr>(group.fd);
    address(52001, network_fixture.peer_ip);
    mock::forbidden = input_address;
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == error(E::e_fault));
    check_binding(kernel, group, false);
    mock::forbidden = payload_address;
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == error(E::e_fault));
    group.port = check_binding(kernel, group, true);
    mock::forbidden = 0;
    expect(invoke(kernel, group, lu::UdpCall::sendto, {fd, payload_address, 3}) == error(E::e_destaddrreq));
    check_binding(kernel, group, true, group.port);
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 1, input_address, 16}) == error(E::e_opnotsupp));
    check_binding(kernel, group, true, group.port);

    extension.submission_status = Status::would_block("synthetic device credit exhausted");
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == error(E::e_again));
    check_binding(kernel, group, true, group.port);
    extension.submission_status = Status::not_initialized("synthetic link down");
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == error(E::e_netdown));
    check_binding(kernel, group, true, group.port);
    extension.submission_status = Status::success();
    std::copy(bytes.begin(), bytes.end(), mock::memory.begin() + 128);
    expect(invoke(kernel, group, lu::UdpCall::sendto,
        {fd, payload_address, 3, 0, input_address, 16}) == 3);
    check_binding(kernel, group, true, group.port);
    std::array<std::byte, net::NativeUdpService::frame_capacity> reply{};
    const auto received = peer.ingest({extension.transmitted.data(), extension.size}, reply); CHECK(received);
    expect(received.value().queued_handle == sender);
    const auto datagram = peer.peek(sender); CHECK(datagram);
    expect(datagram.value().source_port == group.port && datagram.value().payload.size() == bytes.size());
    expect(std::memcmp(datagram.value().payload.data(), bytes.data(), bytes.size()) == 0);
    CHECK(peer.consume(sender));
    packet(kernel, group);
    expect(invoke(kernel, group, lu::UdpCall::recvfrom, {fd, payload_address, 3}) == 3);
    check_binding(kernel, group, true, group.port);
    close_socket(kernel, group);
}

} // namespace

int main()
{
    static Kernel kernel; KernelConfig config{}; config.extension = &extension; CHECK(kernel.boot(config));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(peer.configure({network_fixture.peer_mac, network_fixture.local_mac, network_fixture.peer_ip, network_fixture.local_ip}));
    const auto opened = peer.open(); CHECK(opened); sender = opened.value();
    target_completion_models<lu::Profile::i386>();
    target_completion_models<lu::Profile::x86_64>();
    target_completion_models<lu::Profile::aarch64>();
    target_completion_models<lu::Profile::arm32>();
    target_completion_models<lu::Profile::rv64>();
    target_completion_models<lu::Profile::rv32>();
    target_completion_models<lu::Profile::loongarch64>();
    target_completion_models<lu::Profile::mips>();
    target_completion_models<lu::Profile::mips64>();
    target_completion_models<lu::Profile::ppc>();
    auto group = create(kernel);
    socket_and_bind(kernel, group); send_effects(kernel, group); receive_effects(kernel, group); range_and_descriptor_order(kernel, group);
    bind_zero_runtime(kernel, group); autobind_order_runtime(kernel, group); autobind_persistence_runtime(kernel, group);
    native::release_process(kernel, group.pid); CHECK(kernel.scheduler().exit_process(group.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::runtime_status().ok()); expect(kernel.native_udp().active_sockets() == 0);
    expect(native::entry_stacks.accounting().leased_slots == 0 && mock::owned_hook == nullptr);
    CHECK(peer.close(sender));
    std::printf("LINUX_UDP_RUNTIME_DISPATCH_MODEL_PASS checks=%zu invocations=%zu owned_wrapper_entries=%zu hosted=1 profile=x86_64 guest=0 actual_nic=0\n",
        checks, invocations, mock::entries);
}
