// Actual Kernel poll, scheduler waits and captured native UDP continuation.
// The trap, user mappings, clock and NIC are explicit hosted models.
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_udp_receive_model.hpp"
#include <memory>
#include <source_location>

namespace
{
usize checks{};
void expect(bool value, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "NATIVE_TCP_SHARED_LINK_FAIL line=%u checks=%zu\n", where.line(), checks);
        std::abort();
    }
}
constexpr net::NativeUdpLink link{{2, 0x11, 0x22, 0x33, 0x44, 0x55},
    {2, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}, {192, 0, 2, 2}, {192, 0, 2, 1}};
class Link final : public KernelExtension
{
  public:
    Result<net::NativeUdpTransmitState> network_poll_transmit(Kernel &, usize budget) override
    {
        ++reaps;
        expect(budget == net::NativeTcpEthernetExecutor::budget);
        return failure == StatusCode::ok
            ? Result<net::NativeUdpTransmitState>{net::NativeUdpTransmitState{0, credit}}
            : Result<net::NativeUdpTransmitState>{Status{failure, "hosted original NIC failure"}};
    }
    Result<usize> network_receive(Kernel &, std::span<std::byte>) override
    { ++receives; return Status::would_block(); }
    Status network_try_transmit(Kernel &, std::span<const std::byte>) override
    { ++submissions; return {}; }
    [[nodiscard]] usize calls() const { return reaps + receives + submissions; }
    usize credit{8}, reaps{}, receives{}, submissions{};
    StatusCode failure{StatusCode::ok};
};
struct Group { sched::ProcessId pid{}; };
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("shared-link-host",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 4096})); CHECK(made);
    auto *process = kernel.scheduler().find(made.value()); CHECK(process);
    process->resources().personality = sched::UserPersonality::native;
    process->resources().native_space = {0x100000 + made.value() * 4096, static_cast<u32>(made.value() % 16), 1};
    auto &thread = process->threads()[0];
    CHECK(native::prepare_native_thread(thread)); thread.native_active = true;
    CHECK(kernel.scheduler().set_runnable(made.value()));
    CHECK(kernel.scheduler().activate_thread(made.value(), made.value()));
    CHECK(kernel.posix().activate(made.value()));
    native::runtime_kernel = &kernel; native::started = true;
    return {made.value()};
}
void select(Kernel &kernel, Group group)
{
    CHECK(kernel.scheduler().activate_thread(group.pid, group.pid));
    CHECK(kernel.posix().activate(group.pid));
}
void retire(Kernel &kernel, Group group)
{
    native::release_process(kernel, group.pid);
    CHECK(kernel.scheduler().exit_process(group.pid, 0));
    CHECK(kernel.scheduler().reap_orphans());
    expect(native::runtime_status().ok());
}
void terminal(Kernel &kernel, StatusCode cause)
{
    const auto common = kernel.native_ethernet_executor().state();
    const auto observer = kernel.native_udp_executor().state();
    expect(common.retired && common.credit == 0 && common.controls == 0 && common.cause == cause);
    expect(observer.failed && !observer.active && observer.transmit_credit == 0 && observer.failure_cause == cause);
    expect(kernel.native_tcp().endpoints().pools().retired);
}
void repeat_failure(Kernel &kernel, Link &extension, StatusCode cause)
{
    const auto calls = extension.calls();
    const auto clocks = mock::clock_reads;
    mock::clock_fail = true; mock::clock_failure = StatusCode::io_error;
    for (unsigned repeat = 0; repeat < 3; ++repeat)
    {
        expect(!kernel.poll_network()); terminal(kernel, cause);
    }
    expect(extension.calls() == calls && mock::clock_reads == clocks);
    // The explicit clock API may sample, but cannot replace an earlier cause.
    expect(!kernel.native_tcp_now()); terminal(kernel, cause);
    expect(extension.calls() == calls);
    mock::clock_fail = false;
}
void readiness_exhaustion_wakes_both(Kernel &kernel, Link &extension)
{
    // Replace only the zero-born observer before boot, link configuration or
    // any ownership. Its public small capacity exercises actual saturation.
    auto &observer = const_cast<net::NativeUdpExecutor &>(kernel.native_udp_executor());
    std::destroy_at(&observer);
    std::construct_at(&observer, net::NativeUdpReadinessCapacity{1});
    CHECK(observer.use_shared_link());
    KernelConfig config{}; config.extension = &extension; CHECK(kernel.boot(config));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.native_udp().configure(link));
    CHECK(kernel.native_tcp_now());
    CHECK(kernel.poll_network());
    expect(observer.state().active && observer.state().readiness_sequence == 1 && !observer.state().failed);

    const auto tcp = create(kernel);
    auto listening = kernel.native_tcp_operations().open(tcp.pid); CHECK(listening);
    CHECK(kernel.native_tcp_operations().bind(tcp.pid, listening.value(), {link.local_ip, 55101}));
    CHECK(kernel.native_tcp_operations().listen(tcp.pid, listening.value(), 1));
    const auto tcp_ticket = kernel.native_tcp_waits().arm(tcp.pid, listening.value(), net::tcp::Readiness::accept_interest);
    CHECK(tcp_ticket);
    auto *tcp_thread = kernel.scheduler().find_thread(tcp.pid, tcp.pid); CHECK(tcp_thread);
    tcp_thread->native_active = false; // The TCP side exercises its actual scheduler coordinator only.
    expect(tcp_thread->wait_registration.pending(tcp_ticket.value()));

    const auto udp = create(kernel);
    const auto socket = kernel.native_udp().open(); CHECK(socket);
    CHECK(kernel.native_udp().bind(socket.value(), {}, 55102));
    const auto descriptor = kernel.posix().install_native_udp(socket.value(), false, false); CHECK(descriptor);
    auto *udp_thread = kernel.scheduler().find_thread(udp.pid, udp.pid); CHECK(udp_thread);
    hw::UserFrame frame{}; frame.pc = hw::user_begin + 256; frame.sp = hw::user_begin + 4096;
    frame.registers[14] = OK_SYS_RECVFROM;
    frame.registers[9] = descriptor.value(); frame.registers[10] = hw::user_begin + 128; frame.registers[11] = 8;
    udp_thread->native_frame = frame;
    mock::entry_trap = {hw::TrapKind::syscall, 256}; native::last_pid = native::last_tid = 0;
    expect(native::step(kernel));
    const auto object = native::native_udp_receive_object(udp.pid, udp.pid);
    const auto saved = native::native_udp_receive_objects.resolve(object); CHECK(saved);
    const auto udp_ticket = saved.value()->ticket;
    const auto retained_description = saved.value()->retained_description;
    const auto *description = kernel.scheduler().open_description(retained_description); CHECK(description);
    expect(description->references == 2);
    expect(saved.value()->phase == native::NativeUdpOwnerPhase::published && saved.value()->observed_executor == 1);
    expect(udp_thread->syscall_pending && udp_thread->wait_registration.pending(udp_ticket));

    extension.credit = 4;
    expect(!kernel.poll_network()); terminal(kernel, StatusCode::overflow);
    expect(observer.state().readiness_sequence == 1);
    const auto tcp_completion = kernel.native_tcp_waits().observe(tcp_ticket.value()); CHECK(tcp_completion);
    expect(tcp_completion.value().completion == sched::WaitCompletion::ready);
    expect(tcp_thread->wait_registration.completion() == sched::WaitCompletion::ready);
    CHECK(native::scan_native_udp_receives(kernel));
    expect(udp_thread->wait_registration.ticket() == udp_ticket);
    expect(udp_thread->wait_registration.completion() == sched::WaitCompletion::ready);
    select(kernel, udp);
    const auto entries = mock::entries;
    expect(native::step(kernel));
    expect(mock::entries == entries && !udp_thread->syscall_pending);
    expect(static_cast<i64>(udp_thread->native_frame.registers[14]) == -5);
    expect(!native::native_udp_receive_object(udp.pid, udp.pid).pool);
    description = kernel.scheduler().open_description(retained_description); CHECK(description);
    expect(description->references == 1);
    CHECK(kernel.native_tcp_waits().finish(tcp_ticket.value()));
    expect(kernel.native_tcp_waits().records() == 0);
    repeat_failure(kernel, extension, StatusCode::overflow);
    retire(kernel, udp); retire(kernel, tcp);
    expect(kernel.native_udp().active_sockets() == 0 && kernel.native_tcp().active_streams() == 0);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    const auto owners = native::native_udp_receive_objects.accounting();
    expect(owners.live_objects == 0 && owners.allocations == owners.destructions);
}
void existing_observer_failure(Kernel &kernel, Link &extension)
{
    KernelConfig config{}; config.extension = &extension; CHECK(kernel.boot(config));
    auto &observer = const_cast<net::NativeUdpExecutor &>(kernel.native_udp_executor());
    expect(!observer.observe_shared_link(0, 0, 0, Status::denied("hosted preexisting observer failure")));
    expect(!kernel.native_udp().configured());
    const auto clocks = mock::clock_reads;
    expect(!kernel.poll_network()); terminal(kernel, StatusCode::denied);
    expect(extension.calls() == 0 && mock::clock_reads == clocks);
    repeat_failure(kernel, extension, StatusCode::denied);
}
void driver_failure(Kernel &kernel, Link &extension)
{
    KernelConfig config{}; config.extension = &extension; CHECK(kernel.boot(config));
    CHECK(kernel.native_udp().configure(link)); CHECK(kernel.native_tcp_now());
    extension.failure = StatusCode::fault;
    expect(!kernel.poll_network()); terminal(kernel, StatusCode::fault);
    expect(extension.reaps == 1 && extension.receives == 0 && extension.submissions == 0);
    repeat_failure(kernel, extension, StatusCode::fault);
}
}
int main()
{
    static Kernel exhausted, observed, driver;
    Link exhausted_link, observed_link, driver_link;
    readiness_exhaustion_wakes_both(exhausted, exhausted_link);
    existing_observer_failure(observed, observed_link);
    driver_failure(driver, driver_link);
    std::printf("NATIVE_TCP_SHARED_LINK_HOSTED_PASS checks=%zu real_kernel_poll=1 real_scheduler=1 real_udp_capture=1 guest_execution=0 nic_execution=0\n", checks);
}
