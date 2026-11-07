// Actual runtime owner/scheduler/OFD/copyout code with explicit hosted backends.
// No guest, physical mappings, target privileged capture or NIC executes here.
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_udp_receive_model.hpp"
#include <source_location>
static_assert(lu::configured_native_profile() == lu::Profile::x86_64);

namespace
{
usize checks{};
void expect(bool value, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "NATIVE_UDP_RECEIVE_MODEL_FAIL line=%u checks=%zu\n", where.line(), checks);
        std::abort();
    }
}
using Request = native::SyscallOrigins::NativeUdpReceiveRequest;
constexpr uptr output = hw::user_begin + 128;
constexpr uptr source_address = hw::user_begin + 512;
constexpr uptr source_length = hw::user_begin + 640;
struct Group { sched::ProcessId pid{}; i32 fd{}; u32 endpoint{}; u16 port{}; };
net::NativeUdpService peer;
u32 sender{};
constexpr net::NativeUdpLink udp_link{{2, 0x11, 0x22, 0x33, 0x44, 0x55},
    {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}, {192, 0, 2, 2}, {192, 0, 2, 1}};

Group create(Kernel &kernel, u16 port)
{
    auto made = kernel.scheduler().create_user_process("udp-receive-runtime-model",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 4096})); CHECK(made);
    auto *process = kernel.scheduler().find(made.value());
    process->resources().personality = sched::UserPersonality::native;
    process->resources().native_space = {0x100000 + made.value() * 4096, static_cast<u32>(made.value() % 16), 1};
    auto &thread = process->threads()[0];
    CHECK(native::prepare_native_thread(thread));
    thread.native_active = true;
    CHECK(kernel.scheduler().set_runnable(made.value()));
    CHECK(kernel.scheduler().activate_thread(made.value(), made.value()));
    CHECK(kernel.posix().activate(made.value()));
    auto opened = kernel.native_udp().open(); CHECK(opened);
    CHECK(kernel.native_udp().bind(opened.value(), {}, port));
    auto installed = kernel.posix().install_native_udp(opened.value(), false, false); CHECK(installed);
    native::runtime_kernel = &kernel; native::started = true;
    return {made.value(), installed.value(), opened.value(), port};
}
sched::ThreadControlBlock &thread(Kernel &kernel, Group g)
{ return *kernel.scheduler().find_thread(g.pid, g.pid); }
void select(Kernel &kernel, Group g)
{
    CHECK(kernel.scheduler().activate_thread(g.pid, g.pid));
    CHECK(kernel.posix().activate(g.pid));
}
void retire(Kernel &kernel, Group g)
{
    native::release_process(kernel, g.pid);
    CHECK(kernel.scheduler().exit_process(g.pid, 0));
    CHECK(kernel.scheduler().reap_orphans());
    expect(native::runtime_status().ok());
    expect(native::native_udp_receive_objects.accounting().live_objects == 0);
}
hw::UserFrame frame(Request request)
{
    hw::UserFrame captured{};
    captured.pc = hw::user_begin + 256;
    captured.sp = hw::user_begin + 4096;
    captured.registers[14] = OK_SYS_RECVFROM;
    const std::array<usize, 6> slots{9, 10, 11, 5, 7, 6};
    const std::array<uptr, 6> arguments{static_cast<uptr>(request.fd), request.output, request.count,
        request.flags, request.source_address, request.source_length};
    for (usize at = 0; at < slots.size(); ++at) captured.registers[slots[at]] = arguments[at];
    return captured;
}
memory::ObjectHandle arm(Kernel &kernel, Group g, usize count = 8, bool address = false,
                         sched::ThreadId invoking = 0)
{
    const auto tid = invoking ? invoking : g.pid;
    auto *caller = kernel.scheduler().find_thread(g.pid, tid); CHECK(caller);
    CHECK(kernel.scheduler().activate_thread(g.pid, tid));
    Request request{g.fd, output, count, 0, address ? source_address : 0, address ? source_length : 0,
        address ? 16U : 0U};
    if (address) { constexpr u32 capacity = 16; std::memcpy(mock::memory.data() + 640, &capacity, sizeof(capacity)); }
    caller->native_frame = frame(request);
    mock::entry_trap = {hw::TrapKind::syscall, 256};
    native::last_pid = native::last_tid = 0;
    const auto entries = mock::entries;
    expect(native::step(kernel));
    expect(mock::entries == entries + 1);
    const auto object = native::native_udp_receive_object(g.pid, tid);
    expect(object.pool != nullptr);
    const auto saved = native::native_udp_receive_objects.resolve(object); CHECK(saved);
    expect(saved.value()->phase == native::NativeUdpOwnerPhase::published);
    expect(caller->syscall_pending && caller->wait_registration.pending(saved.value()->ticket));
    return object;
}
void packet(Kernel &kernel, u16 port, std::span<const std::byte> payload)
{
    std::array<std::byte, net::NativeUdpService::frame_capacity> bytes{}, reply{};
    const auto built = peer.build_datagram(sender, udp_link.local_ip, port, payload, bytes); CHECK(built);
    const auto ingested = kernel.native_udp().ingest({bytes.data(), built.value()}, reply); CHECK(ingested);
    expect(ingested.value().queued_handle != 0);
}
constexpr std::array<std::byte, 3> payload{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
i64 result(Kernel &kernel, Group g)
{ return static_cast<i64>(thread(kernel, g).native_frame.registers[14]); }
void resume(Kernel &kernel, Group g, i64 wanted)
{
    const auto entries = mock::entries;
    expect(native::step(kernel));
    expect(mock::entries == entries);
    expect(!thread(kernel, g).syscall_pending);
    expect(!native::native_udp_receive_object(g.pid, g.pid).pool);
    expect(result(kernel, g) == wanted);
}

void copied_capacity_and_double_cleanup(Kernel &kernel)
{
    const auto g = create(kernel, 51001);
    const auto object = arm(kernel, g, 2, true);
    constexpr u32 changed = 0;
    std::memcpy(mock::memory.data() + 640, &changed, sizeof(changed));
    packet(kernel, g.port, payload);
    resume(kernel, g, 2);
    expect(mock::memory[128] == std::byte{'a'} && mock::memory[129] == std::byte{'b'});
    u32 actual{}; std::memcpy(&actual, mock::memory.data() + 640, sizeof(actual));
    expect(actual == 16);
    expect(kernel.native_udp().peek(g.endpoint).status().code() == StatusCode::would_block);
    const auto live = native::native_udp_receive_objects.accounting().live_objects;
    native::retire_native_udp_receive(kernel, object);
    expect(native::native_udp_receive_objects.accounting().live_objects == live);
    retire(kernel, g);
}
void preflight_fault_preserves_packet(Kernel &kernel)
{
    const auto g = create(kernel, 51002);
    arm(kernel, g, 3, true);
    packet(kernel, g.port, payload);
    const auto outputs = mock::outputs;
    mock::write_forbidden = source_address;
    resume(kernel, g, -14);
    expect(mock::outputs == outputs);
    expect(kernel.native_udp().peek(g.endpoint).ok());
    mock::write_forbidden = 0;
    select(kernel, g);
    bool complete = true;
    const auto value = native::dispatch(kernel, g.pid, {OK_SYS_RECVFROM, {static_cast<uptr>(g.fd), output, 3, 0, 0, 0}}, complete);
    expect(complete && value == 3);
    expect(kernel.native_udp().peek(g.endpoint).status().code() == StatusCode::would_block);
    retire(kernel, g);
}
void rearm_after_other_consumer(Kernel &kernel)
{
    const auto g = create(kernel, 51003);
    const auto object = arm(kernel, g);
    const auto saved = native::native_udp_receive_objects.resolve(object); CHECK(saved);
    const auto old_ticket = saved.value()->ticket;
    const auto request = saved.value()->request;
    packet(kernel, g.port, payload);
    CHECK(native::scan_native_udp_receives(kernel));
    CHECK(kernel.native_udp().consume(g.endpoint));
    const auto entries = mock::entries;
    expect(native::step(kernel));
    expect(mock::entries == entries);
    const auto rearmed = native::native_udp_receive_objects.resolve(object); CHECK(rearmed);
    expect(rearmed.value()->ticket.generation > old_ticket.generation);
    expect(rearmed.value()->request == request && rearmed.value()->origin_binding->ticket == rearmed.value()->ticket);
    expect(thread(kernel, g).wait_registration.pending(rearmed.value()->ticket));
    expect(!kernel.scheduler().complete_wait(old_ticket, sched::WaitCompletion::cancelled));
    packet(kernel, g.port, payload);
    resume(kernel, g, 3);
    retire(kernel, g);
}
void descriptor_reuse_keeps_endpoint(Kernel &kernel)
{
    const auto g = create(kernel, 51004);
    arm(kernel, g);
    // Readiness makes this process legally runnable before its fd table changes.
    packet(kernel, g.port, payload);
    CHECK(native::scan_native_udp_receives(kernel));
    CHECK(kernel.posix().activate(g.pid));
    const auto other = kernel.native_udp().open(); CHECK(other);
    CHECK(kernel.native_udp().bind(other.value(), {}, 51005));
    const auto other_fd = kernel.posix().install_native_udp(other.value()); CHECK(other_fd);
    CHECK(kernel.posix().duplicate_to(other_fd.value(), g.fd));
    expect(kernel.native_udp().active_sockets() == 2);
    packet(kernel, 51005, payload);
    resume(kernel, g, 3);
    expect(kernel.native_udp().readiness(g.endpoint).status().code() == StatusCode::bad_descriptor);
    expect(kernel.native_udp().peek(other.value()).ok());
    expect(kernel.native_udp().active_sockets() == 1);
    retire(kernel, g);
}
void zero_length_and_completion_causes(Kernel &kernel)
{
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        const auto g = create(kernel, static_cast<u16>(51006 + mode));
        const auto object = arm(kernel, g, 0);
        const auto saved = native::native_udp_receive_objects.resolve(object); CHECK(saved);
        if (mode == 0) packet(kernel, g.port, {});
        else CHECK(kernel.scheduler().complete_wait(saved.value()->ticket,
            mode == 1 ? sched::WaitCompletion::cancelled : sched::WaitCompletion::interrupted));
        resume(kernel, g, mode == 0 ? 0 : mode == 1 ? -125 : -4);
        if (mode == 0) expect(kernel.native_udp().peek(g.endpoint).status().code() == StatusCode::would_block);
        retire(kernel, g);
    }
}
void retirement_and_reused_object(Kernel &kernel)
{
    const auto first = create(kernel, 51009);
    const auto old = arm(kernel, first);
    native::retire_native_udp_receives(kernel, first.pid);
    native::retire_native_udp_receives(kernel, first.pid);
    expect(!native::native_udp_receive_object(first.pid, first.pid).pool);
    retire(kernel, first);
    const auto next = create(kernel, 51010);
    const auto current = arm(kernel, next);
    expect(old != current);
    native::retire_native_udp_receive(kernel, old);
    expect(native::native_udp_receive_object(next.pid, next.pid) == current);
    packet(kernel, next.port, payload);
    resume(kernel, next, 3);
    retire(kernel, next);
}
void committed_exec_retirement_order(Kernel &kernel)
{
    // Exercise the public replacement engine and real scheduler commit, without
    // loading an ELF or claiming a guest exec. No private capture state is edited.
    for (unsigned mode = 0; mode < 2; ++mode)
    {
        const auto g = create(kernel, static_cast<u16>(51011 + mode));
        auto *process = kernel.scheduler().find(g.pid); CHECK(process);
        thread(kernel, g).native_active = false;
        const auto sibling = kernel.scheduler().create_thread_prepared(g.pid,
            kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 4096}),
            [](sched::ThreadControlBlock &created) { return native::prepare_native_thread(created); });
        CHECK(sibling);
        auto *waiting = kernel.scheduler().find_thread(g.pid, sibling.value()); CHECK(waiting);
        waiting->native_active = true;
        const auto old_stack = waiting->entry_stack;
        const memory::KernelEntryStackOwner old_owner{g.pid, sibling.value()};
        arm(kernel, g, 8, false, sibling.value());
        CHECK(kernel.scheduler().activate_thread(g.pid, g.pid));
        const auto leases = native::entry_stacks.accounting().leased_slots;
        bool retired_before_release = false;
        const auto replaced = native::replace_entry_stacks(native::entry_stacks, *process,
            [&] { return kernel.scheduler().replace_image(g.pid,
                kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 4096}), {}); },
            [&]() -> Status {
                CHECK(native::entry_stacks.top(old_stack, old_owner));
                const auto status = native::retire_native_udp_receives_for_exec(kernel, g.pid);
                if (!status) return status;
                expect(!native::native_udp_receive_object(g.pid, sibling.value()).pool);
                expect(native::native_udp_receive_objects.accounting().live_objects == 0);
                CHECK(native::entry_stacks.top(old_stack, old_owner));
                retired_before_release = true;
                return mode ? Status::io_error("hosted subsequent retirement failure") : Status{};
            });
        expect(replaced.committed && retired_before_release);
        if (mode)
        {
            expect(!replaced && replaced.retained_count == 1);
            expect(native::entry_stacks.accounting().leased_slots == leases);
            CHECK(native::entry_stacks.top(old_stack, old_owner));
            CHECK(native::entry_stacks.release(replaced.retained[0].handle, replaced.retained[0].owner));
        }
        else
        {
            expect(static_cast<bool>(replaced) && replaced.retained_count == 0);
            expect(!native::entry_stacks.top(old_stack, old_owner));
            expect(native::entry_stacks.accounting().leased_slots == leases - 1);
        }
        retire(kernel, g);
    }
}
// Native AF_INET6 takes exact 28-byte records; the IPv4 socket keeps its own.
// The connected peer's port is unreachable (an ICMP error the executor
// routed): a blocked receiver wakes with ECONNREFUSED, poll reports POLLERR,
// and the error is reported once (Linux __udp4_lib_err). The send side is
// covered by net_udp_tests: native sendto needs an extension and an address.
void icmp_refusal_wakes_receiver(Kernel &kernel)
{
    const auto g = create(kernel, 51030);
    // Earlier cases bound the sender; connect to it so its datagrams pass the peer filter.
    const auto source = peer.binding(sender); CHECK(source && source.value().bound);
    const u16 remote = source.value().port;
    CHECK(kernel.native_udp().connect(g.endpoint, udp_link.peer_ip, remote));
    const net::wire::IcmpErrorClass refused{net::wire::IcmpErrorKind::refused, true, 0};
    const auto refuse = [&] {
        auto taken = kernel.native_udp().icmp_error(udp_link.local_ip, g.port, udp_link.peer_ip, remote, refused);
        expect(taken && taken.value());
    };
    arm(kernel, g);
    refuse();
    resume(kernel, g, -OK_ECONNREFUSED);
    select(kernel, g);
    auto call = [&](uptr number, std::array<uptr, 6> arguments) {
        bool complete = true;
        const auto value = native::dispatch(kernel, g.pid, {number, arguments}, complete);
        expect(complete); return value;
    };
    // Reported once: a zero-timeout poll then finds nothing pending.
    constexpr uptr fds = hw::user_begin + 896;
    const auto poll = [&](i16 &revents) {
        ok_pollfd entry{g.fd, OK_POLLIN, 0}; std::memcpy(mock::memory.data() + 896, &entry, sizeof(entry));
        const auto ready = call(OK_SYS_POLL, {fds, 1, 0, 0, 0, 0});
        std::memcpy(&entry, mock::memory.data() + 896, sizeof(entry)); revents = entry.revents; return ready;
    };
    i16 revents{};
    expect(poll(revents) == 0 && revents == 0);
    refuse();
    expect(poll(revents) == 1 && revents == OK_POLLERR);
    expect(call(OK_SYS_RECVFROM, {static_cast<uptr>(g.fd), output, 8, 0, 0, 0}) == -OK_ECONNREFUSED);
    expect(poll(revents) == 0 && revents == 0);
    // A datagram queued after the error is still read after it.
    refuse(); packet(kernel, g.port, payload);
    expect(call(OK_SYS_RECVFROM, {static_cast<uptr>(g.fd), output, 8, 0, 0, 0}) == -OK_ECONNREFUSED);
    expect(call(OK_SYS_RECVFROM, {static_cast<uptr>(g.fd), output, 8, 0, 0, 0}) == 3);
    retire(kernel, g);
}
// A blocking poll over UDP sockets sleeps on their wait queues: an idle pass
// does not evaluate it, and a datagram signals the queue so the next pass
// completes it. Asking for POLLOUT keeps the rescan, since writability
// follows transmit credit, which signals no queue.
native::PollWait *poll_wait(sched::ProcessId pid)
{
    for (const auto &entry : native::poll_waits)
        if (entry.pid == pid)
            if (auto resolved = native::poll_objects.resolve(entry.object)) return resolved.value();
    return nullptr;
}
void poll_sleeps_on_socket_queue(Kernel &kernel)
{
    const auto g = create(kernel, 51040);
    auto &caller = thread(kernel, g);
    constexpr uptr fds = hw::user_begin + 896;
    const auto arm_poll = [&](i16 events) {
        ok_pollfd entry{g.fd, events, 0}; std::memcpy(mock::memory.data() + 896, &entry, sizeof(entry));
        CHECK(kernel.scheduler().activate_thread(g.pid, g.pid));
        hw::UserFrame captured{}; captured.pc = hw::user_begin + 256; captured.sp = hw::user_begin + 4096;
        captured.registers[14] = OK_SYS_POLL; captured.registers[9] = fds; captured.registers[10] = 1;
        captured.registers[11] = ~uptr{0};
        caller.native_frame = captured; mock::entry_trap = {hw::TrapKind::syscall, 256};
        native::last_pid = native::last_tid = 0;
        expect(native::step(kernel)); expect(caller.syscall_pending);
        // A completed poll may resume in the same pass; let it take a timer trap.
        mock::entry_trap = {hw::TrapKind::timer};
        return poll_wait(g.pid);
    };
    auto *wait = arm_poll(OK_POLLIN);
    expect(wait && wait->socket_only && !wait->dirty);
    for (usize pass = 0; pass < 3; ++pass)
    {
        expect(native::step(kernel)); expect(caller.syscall_pending && poll_wait(g.pid) == wait);
        expect(wait->socket_only && !wait->dirty);
    }
    packet(kernel, g.port, payload);
    expect(native::step(kernel));
    expect(!caller.syscall_pending && result(kernel, g) == 1 && !poll_wait(g.pid));
    ok_pollfd entry{}; std::memcpy(&entry, mock::memory.data() + 896, sizeof(entry));
    expect(entry.revents == OK_POLLIN);
    CHECK(kernel.native_udp().consume(g.endpoint));
    wait = arm_poll(OK_POLLIN | OK_POLLOUT);
    expect(wait && !wait->socket_only);
    expect(native::step(kernel)); expect(caller.syscall_pending);
    packet(kernel, g.port, payload);
    expect(native::step(kernel));
    expect(!caller.syscall_pending && result(kernel, g) == 1);
    retire(kernel, g);
}
void ipv6_native_calls(Kernel &kernel)
{
    const auto g = create(kernel, 51020);
    select(kernel, g);
    auto call = [&](uptr number, std::array<uptr, 6> arguments) {
        bool complete = true;
        const auto value = native::dispatch(kernel, g.pid, {number, arguments}, complete);
        expect(complete); return value;
    };
    constexpr uptr record = hw::user_begin + 768;
    const auto self = net::wire::link_local_from_mac(udp_link.local_mac);
    const auto router = net::wire::link_local_from_mac(udp_link.peer_mac);
    const auto put = [](u16 family, u16 port, const net::wire::Ipv6 &address) {
        ok_sockaddr_in6 value{family, {static_cast<u8>(port >> 8), static_cast<u8>(port)}, {1, 2, 3, 4}, {}, 9};
        for (usize at = 0; at < 16; ++at) value.address[at] = address[at];
        std::memcpy(mock::memory.data() + 768, &value, sizeof(value));
    };
    expect(call(OK_SYS_SOCKET, {3, OK_SOCK_DGRAM, 0, 0, 0, 0}) == -22);
    const auto fd = call(OK_SYS_SOCKET, {OK_AF_INET6, OK_SOCK_DGRAM | OK_SOCK_NONBLOCK, OK_IPPROTO_UDP, 0, 0, 0});
    expect(fd >= 3);
    const net::wire::Ipv6 mapped{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 2};
    put(OK_AF_INET6, 51021, {});
    expect(call(OK_SYS_BIND, {static_cast<uptr>(fd), record, sizeof(ok_sockaddr_in), 0, 0, 0}) == -OK_EINVAL);
    expect(call(OK_SYS_BIND, {static_cast<uptr>(fd), record, 24, 0, 0, 0}) == -OK_EINVAL);
    put(OK_AF_INET, 51021, {});
    expect(call(OK_SYS_BIND, {static_cast<uptr>(fd), record, sizeof(ok_sockaddr_in6), 0, 0, 0}) == -OK_EAFNOSUPPORT);
    put(OK_AF_INET6, 51021, mapped);
    expect(call(OK_SYS_BIND, {static_cast<uptr>(fd), record, sizeof(ok_sockaddr_in6), 0, 0, 0}) == -OK_EINVAL);
    // An IPv4 socket keeps its exact sixteen-byte AF_INET rule.
    put(OK_AF_INET6, 51021, {});
    expect(call(OK_SYS_BIND, {static_cast<uptr>(g.fd), record, sizeof(ok_sockaddr_in6), 0, 0, 0}) == -OK_EINVAL);
    expect(call(OK_SYS_BIND, {static_cast<uptr>(fd), record, sizeof(ok_sockaddr_in6), 0, 0, 0}) == 0);
    std::array<std::byte, 128> packet{};
    const auto built = net::wire::build_udp<net::wire::Ipv6Family>(packet, router, self, 41000, 51021, 0, 64, payload); CHECK(built);
    const auto queued = kernel.native_udp().deliver({packet.data(), built.value()});
    expect(queued && queued.value().kind == net::NativeUdpFrameKind::udp_queued);
    u32 capacity = sizeof(ok_sockaddr_in);
    std::memcpy(mock::memory.data() + 640, &capacity, sizeof(capacity));
    expect(call(OK_SYS_RECVFROM, {static_cast<uptr>(fd), output, 8, 0, source_address, source_length}) == -OK_EINVAL);
    capacity = sizeof(ok_sockaddr_in6);
    std::memcpy(mock::memory.data() + 640, &capacity, sizeof(capacity));
    expect(call(OK_SYS_RECVFROM, {static_cast<uptr>(fd), output, 8, 0, source_address, source_length}) == 3);
    ok_sockaddr_in6 source{}; std::memcpy(&source, mock::memory.data() + 512, sizeof(source));
    u32 actual{}; std::memcpy(&actual, mock::memory.data() + 640, sizeof(actual));
    expect(actual == 28 && source.family == OK_AF_INET6 && source.port_be[0] == (41000 >> 8) && source.port_be[1] == (41000 & 0xff));
    expect(source.scope_id == 1 && source.flowinfo_be[0] == 0 && std::memcmp(source.address, router.data(), 16) == 0);
    expect(call(OK_SYS_RECVFROM, {static_cast<uptr>(fd), output, 8, 0, 0, 0}) == -11);
    retire(kernel, g);
}
}
int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.native_udp().configure(udp_link));
    CHECK(peer.configure({udp_link.peer_mac, udp_link.local_mac, udp_link.peer_ip, udp_link.local_ip}));
    const auto opened = peer.open(); CHECK(opened); sender = opened.value();
    copied_capacity_and_double_cleanup(kernel);
    preflight_fault_preserves_packet(kernel);
    rearm_after_other_consumer(kernel);
    descriptor_reuse_keeps_endpoint(kernel);
    zero_length_and_completion_causes(kernel);
    retirement_and_reused_object(kernel);
    committed_exec_retirement_order(kernel);
    ipv6_native_calls(kernel);
    icmp_refusal_wakes_receiver(kernel);
    poll_sleeps_on_socket_queue(kernel);
    expect(kernel.native_udp().active_sockets() == 0);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    const auto owners = native::native_udp_receive_objects.accounting();
    expect(owners.allocations == owners.destructions && owners.live_objects == 0);
    CHECK(peer.close(sender));
    std::printf("NATIVE_UDP_RECEIVE_MODEL_PASS checks=%zu hosted=1 guest=0 actual_nic=0 owned_wrapper_entries=%zu owners=%llu\n",
        checks, mock::entries, static_cast<unsigned long long>(owners.allocations));
}
