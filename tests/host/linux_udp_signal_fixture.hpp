#pragma once
// Actual captured Linux receive and signal runtime; hosted traps/mappings only.
#define OK_NATIVE_SIGNAL_RUNTIME 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_LINUX_UDP_SIGNAL_FAILURE_FIXTURE 1
#include "native_udp_receive_model.hpp"
#include <source_location>
static_assert(lu::configured_native_profile() == lu::Profile::x86_64);
namespace ok::memory
{
struct ObjectPoolTestAccess
{
    template <typename Pool> static void short_release_lease(Pool &pool, ObjectHandle object)
    {
        auto &lease=pool.pages_[object.page_slot].lease;
        lease.bytes={lease.bytes.data(),lease.bytes.size()-1};
    }
};
}
namespace
{
using C = lu::UdpConstants<lu::Profile::x86_64>;
constexpr uptr output = hw::user_begin + 128;
constexpr uptr site = hw::user_begin + 1024, handler = hw::user_begin + 2048;
constexpr net::NativeUdpLink network_link{{2,1,2,3,4,5},{2,6,7,8,9,10},{192,0,2,2},{192,0,2,1}};
constexpr std::array<std::byte,3> bytes{std::byte{'a'},std::byte{'b'},std::byte{'c'}};
net::NativeUdpService peer;
u32 sender{};
usize checks{}, cases{}, captures{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr,"LINUX_UDP_SIGNAL_FAIL line=%u checks=%zu\n",at.line(),checks); std::abort(); }
}
struct Group { sched::ProcessId pid{}; i32 fd{-1}; u16 description{},port{}; u32 endpoint{}; };
auto &process(Kernel &kernel, Group g) { return *kernel.scheduler().find(g.pid); }
auto &thread(Kernel &kernel, Group g) { return *kernel.scheduler().find_thread(g.pid,g.pid); }
Group create(Kernel &kernel)
{
    const auto made=kernel.scheduler().create_user_process("linux-udp-signal-host",
        kernel.arch().make_user_context({hw::user_begin,hw::user_begin+8192})); CHECK(made);
    Group g{made.value(),-1,0,static_cast<u16>(57000+cases++)};
    auto &p=process(kernel,g); p.resources().personality=sched::UserPersonality::linux;
    p.resources().native_space={0x100000+g.pid*4096,static_cast<u32>(g.pid%16),1};
    CHECK(native::allocate(p.resources().native_pages,hw::user_begin,hw::page_read|hw::page_execute));
    auto &t=thread(kernel,g); CHECK(native::prepare_native_thread(t)); t.native_active=true;
    CHECK(kernel.scheduler().set_runnable(g.pid)); CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
    CHECK(kernel.posix().activate(g.pid));
    const auto endpoint=kernel.native_udp().open(); CHECK(endpoint); g.endpoint=endpoint.value();
    CHECK(kernel.native_udp().bind(g.endpoint,{},g.port));
    const auto fd=kernel.posix().install_native_udp(g.endpoint,false,false); CHECK(fd); g.fd=fd.value();
    g.description=p.fd_table().entry(g.fd)->description;
    native::runtime_kernel=&kernel; native::started=true; CHECK(native::runtime_status());
    return g;
}
memory::ObjectHandle arm(Kernel &kernel, Group g)
{
    CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    hw::UserFrame frame{}; frame.flags=0x202; frame.pc=site+2; frame.sp=hw::user_begin+8192;
    const auto fp=native::X86SignalFrame::reset_fp();
    for(usize i=0;i<fp.size();++i) frame.fp_state[i]=static_cast<u8>(fp[i]);
    frame.registers[14]=*C::syscall_number(lu::UdpCall::recvfrom);
    frame.registers[9]=g.fd; frame.registers[10]=output; frame.registers[11]=8;
    t.native_frame=frame; t.syscall_pending=false;
    const auto active=native::entry_stacks.activate_entry(t.entry_stack,{g.pid,g.pid},0); CHECK(active);
    mock::entry_trap={hw::TrapKind::syscall,256};
    const auto observed=native::shared_capture.enter(native::syscall_origins,native::vfs_continuations,
        p,t,active.value(),kernel.scheduler()); CHECK(observed);
    CHECK(native::entry_stacks.deactivate_entry(t.entry_stack,{g.pid,g.pid},0));
    t.syscall_pending=true; ++captures;
    CHECK(native::dispatch_linux_udp(kernel,p,t));
    const auto object=native::linux_udp_receive_object(g.pid,g.pid); CHECK(object.pool);
    const auto owner=native::linux_udp_receive_objects.resolve(object); CHECK(owner);
    expect(t.wait_registration.pending(owner.value()->ticket) && t.syscall_pending);
    expect(native::private_signal_admit_blocked(kernel,p,t));
    expect(kernel.scheduler().open_description(g.description)->references==2);
    return object;
}
void caught(Kernel &kernel, Group g, u32 signal=10, bool restart_flag=false)
{
    const auto actions=native::private_signal_table(kernel,g.pid); CHECK(actions);
    auto &action=*actions->find(signal); action.handler=handler; action.restorer=handler+4;
    action.flags=0x04000004 | (restart_flag ? 0x10000000 : 0);
}
void enqueue(Kernel &kernel, Group g, u32 signal=10, bool shared=false)
{
    if(shared) expect(native::private_process_kill(kernel,process(kernel,g),g.pid,signal)==0);
    else
    {
        const auto queue=native::private_signal_queue(thread(kernel,g)); CHECK(queue);
        expect(queue->enqueue({signal,-6,static_cast<u32>(g.pid),0})==native::PrivateSignalQueue::Enqueue::queued);
    }
}
void packet(Kernel &kernel, Group g)
{
    std::array<std::byte,net::NativeUdpService::frame_capacity> frame{},reply{};
    const auto built=peer.build_datagram(sender,network_link.local_ip,g.port,bytes,frame); CHECK(built);
    const auto received=kernel.native_udp().ingest({frame.data(),built.value()},reply); CHECK(received);
    expect(received.value().queued_handle==g.endpoint);
}
void release(Kernel &kernel, Group g)
{
    native::release_process(kernel,g.pid); CHECK(native::runtime_status());
    CHECK(kernel.scheduler().exit_process(g.pid,0)); CHECK(kernel.scheduler().reap_orphans());
    expect(!native::linux_udp_receive_object(g.pid,g.pid).pool &&
        native::linux_udp_receive_objects.accounting().live_objects==0);
}
[[maybe_unused]] void saved_result(Kernel &kernel, Group g, i64 result)
{
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    expect(native::private_signal_deliver(kernel,p,t));
    expect(t.native_frame.pc==handler && t.native_frame.registers[9]==10);
    std::array<std::byte,native::X86SignalFrame::prefix_bytes> saved{};
    CHECK(hw::copy_from_user(p.resources().native_space,saved.data(),t.native_frame.sp,saved.size()));
    expect(native::X86SignalFrame::get(saved,native::X86SignalFrame::context_offset+128,8)==site+2);
    expect(native::X86SignalFrame::get(saved,native::X86SignalFrame::context_offset+104,8)==static_cast<u64>(result));
    expect(!native::private_signal_pending(kernel,p,t).pending_mask().contains(10));
}
}
