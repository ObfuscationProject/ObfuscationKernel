// Actual private concrete dispatch, captured owner, step selection and lifecycle.
// Synthetic traps, flat mappings and peer frames are hosted controls only.
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_NATIVE_SIGNAL_RUNTIME 1
#include "native_udp_receive_model.hpp"
#include <source_location>
static_assert(lu::configured_native_profile() == lu::Profile::x86_64);
namespace
{
using C = lu::UdpConstants<lu::Profile::x86_64>;
using E = lu::UdpErrno;
constexpr uptr output = hw::user_begin + 128, input = hw::user_begin + 512;
constexpr net::NativeUdpLink network_link{{2,1,2,3,4,5},{2,6,7,8,9,10},{192,0,2,2},{192,0,2,1}};
constexpr std::array<std::byte,3> data{std::byte{'a'},std::byte{'b'},std::byte{'c'}};
net::NativeUdpService peer;
u32 sender{};
usize checks{}, calls{}, resumes{}, groups{};
void expect(bool value, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr,"LINUX_UDP_DISPATCH_INTEGRATION_FAIL line=%u checks=%zu\n",where.line(),checks);
        std::abort();
    }
}
i64 error(E value) { return -static_cast<i64>(*C::errno_number(value)); }
struct Group { sched::ProcessId pid{}; i32 fd{-1}; u16 description{}, port{}; u32 endpoint{}; };
auto &process(Kernel &kernel, Group g) { return *kernel.scheduler().find(g.pid); }
auto &thread(Kernel &kernel, Group g, sched::ThreadId tid = 0)
{ return *kernel.scheduler().find_thread(g.pid,tid ? tid : g.pid); }
void select(Kernel &kernel, Group g, sched::ThreadId tid = 0)
{
    const auto selected=tid ? tid : g.pid;
    CHECK(kernel.scheduler().activate_thread(g.pid,selected));
    expect(kernel.scheduler().current_thread()==kernel.scheduler().find_thread(g.pid,selected));
}
Group create(Kernel &kernel, u16 port)
{
    const auto created = kernel.scheduler().create_user_process("linux-udp-s2-host",
        kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096})); CHECK(created);
    Group g{created.value(),-1,0,port};
    auto &p = process(kernel,g); p.resources().personality=sched::UserPersonality::linux;
    p.resources().native_space={0x100000+g.pid*4096,static_cast<u32>(g.pid%16),1};
    auto &t=thread(kernel,g); CHECK(native::prepare_native_thread(t)); t.native_active=true;
    CHECK(kernel.scheduler().set_runnable(g.pid)); select(kernel,g);
    native::runtime_kernel=&kernel; native::started=true; CHECK(native::runtime_status()); ++groups;
    return g;
}
// The frame is presented to the genuine shared capture boundary. No origin,
// frame history, backend callback or capture counter is manually admitted.
void capture(Kernel &kernel, Group g, lu::UdpCall call, std::array<uptr,6> args,
            sched::ThreadId tid = 0)
{
    select(kernel,g,tid); auto &p=process(kernel,g); auto &t=thread(kernel,g,tid);
    expect(!t.syscall_pending && !native::linux_udp_receive_object(g.pid,t.tid).pool);
    hw::UserFrame frame{}; frame.pc=hw::user_begin+256; frame.sp=hw::user_begin+4096;
    frame.registers[14]=*C::syscall_number(call);
    constexpr std::array<usize,6> slots{9,10,11,5,7,6};
    for (usize i=0;i<slots.size();++i) frame.registers[slots[i]]=args[i];
    t.native_frame=frame;
    auto active=native::entry_stacks.activate_entry(t.entry_stack,{g.pid,t.tid},0); CHECK(active);
    mock::entry_trap={hw::TrapKind::syscall,256}; expect(mock::owned_hook==nullptr);
    const auto entries=mock::entries;
    const auto captured=native::shared_capture.enter(native::syscall_origins,native::vfs_continuations,
        p,t,active.value(),kernel.scheduler()); CHECK(captured);
    CHECK(native::entry_stacks.deactivate_entry(t.entry_stack,{g.pid,t.tid},0));
    expect(mock::entries==entries+1); t.syscall_pending=true; ++calls;
}
void invoke(Kernel &kernel, Group g, lu::UdpCall call, std::array<uptr,6> args,
            sched::ThreadId tid = 0)
{
    capture(kernel,g,call,args,tid);
    CHECK(native::dispatch_linux_udp(kernel,process(kernel,g),thread(kernel,g,tid)));
    expect(native::runtime_status().ok());
}
i64 result(Kernel &kernel, Group g, sched::ThreadId tid = 0)
{ return static_cast<i64>(thread(kernel,g,tid).native_frame.registers[14]); }
void socket(Kernel &kernel, Group &g, bool nonblock=false, bool cloexec=false)
{
    invoke(kernel,g,lu::UdpCall::socket,{C::af_inet,C::sock_dgram|(nonblock?C::sock_nonblock:0)|
        (cloexec?C::sock_cloexec:0),C::protocol_udp});
    expect(!thread(kernel,g).syscall_pending && result(kernel,g)>=3);
    g.fd=static_cast<i32>(result(kernel,g));
    const auto *slot=process(kernel,g).fd_table().entry(g.fd); CHECK(slot&&slot->used);
    g.description=slot->description;
    const auto *open=kernel.scheduler().open_description(g.description); CHECK(open);
    g.endpoint=open->native_udp_handle;
    expect(bool(open->flags&posix::o_NONBLOCK)==nonblock && slot->close_on_exec==cloexec && open->references==1);
    std::array<u8,16> address{}; CHECK(lu::UdpWire<lu::Profile::x86_64>::encode_address(address,{2,g.port,{}}));
    std::memcpy(mock::memory.data()+512,address.data(),address.size());
    invoke(kernel,g,lu::UdpCall::bind,{static_cast<uptr>(g.fd),input,16});
    expect(!thread(kernel,g).syscall_pending && result(kernel,g)==0);
}
memory::ObjectHandle arm(Kernel &kernel, Group g, sched::ThreadId tid=0, u32 flags=0)
{
    invoke(kernel,g,lu::UdpCall::recvfrom,{static_cast<uptr>(g.fd),output,8,flags},tid);
    auto &t=thread(kernel,g,tid);
    const auto object=native::linux_udp_receive_object(g.pid,t.tid); CHECK(object.pool);
    const auto owned=native::linux_udp_receive_objects.resolve(object); CHECK(owned);
    expect(t.syscall_pending && t.wait_registration.pending(owned.value()->ticket));
    expect(kernel.scheduler().open_description(g.description)->references>=2);
    auto immediate=native::LinuxUdpImmediateAccess<lu::Profile::x86_64>::acquire(native::syscall_origins,process(kernel,g),t);
    expect(!immediate && native::private_signal_admit_blocked(kernel,process(kernel,g),t));
    expect(native::retire_native_thread(t).code()==StatusCode::busy);
    expect(native::prepare_native_thread(t).code()==StatusCode::busy);
    return object;
}
void packet(Kernel &kernel, Group g)
{
    std::array<std::byte,net::NativeUdpService::frame_capacity> frame{},reply{};
    const auto built=peer.build_datagram(sender,network_link.local_ip,g.port,data,frame); CHECK(built);
    const auto received=kernel.native_udp().ingest({frame.data(),built.value()},reply); CHECK(received);
    expect(received.value().queued_handle==g.endpoint);
}
void drive(Kernel &kernel)
{
    const auto entries=mock::entries, decoded=mock::syscall_decodes, inputs=mock::copies;
    native::last_pid=native::last_tid=0;
    CHECK(native::step(kernel)); ++resumes;
    expect(mock::entries==entries && mock::syscall_decodes==decoded && mock::copies==inputs);
    expect(native::runtime_status().ok());
}
void release(Kernel &kernel, Group g)
{
    native::release_process(kernel,g.pid);
    expect(native::runtime_status().ok() && !native::linux_udp_receive_object(g.pid,g.pid).pool);
    expect(!thread(kernel,g).entry_stack.pool && !process(kernel,g).resources().native_space.root);
    CHECK(kernel.scheduler().exit_process(g.pid,0)); CHECK(kernel.scheduler().reap_orphans());
    expect(!kernel.native_udp().binding(g.endpoint));
}
void plain_nonblock_and_idle(Kernel &kernel)
{
    auto g=create(kernel,56001); socket(kernel,g);
    invoke(kernel,g,lu::UdpCall::recvfrom,{static_cast<uptr>(g.fd),output,8,*C::message(lu::UdpMessage::dontwait)});
    expect(!thread(kernel,g).syscall_pending && result(kernel,g)==error(E::e_again));
    const auto object=arm(kernel,g); const auto entries=mock::entries,decodes=mock::syscall_decodes;
    expect(!native::step(kernel)); // Advisory idle result; the external poll loop continues.
    expect(mock::entries==entries && mock::syscall_decodes==decodes && thread(kernel,g).syscall_pending);
    packet(kernel,g); drive(kernel);
    expect(!thread(kernel,g).syscall_pending && result(kernel,g)==3 && !native::linux_udp_receive_objects.resolve(object));
    expect(mock::memory[128]==std::byte{'a'} && mock::memory[130]==std::byte{'c'});
    expect(kernel.scheduler().open_description(g.description)->references==1); release(kernel,g);
    g=create(kernel,56002); socket(kernel,g,true,true);
    invoke(kernel,g,lu::UdpCall::recvfrom,{static_cast<uptr>(g.fd),output,8});
    expect(!thread(kernel,g).syscall_pending && result(kernel,g)==error(E::e_again)); release(kernel,g);
}
void readiness_stolen(Kernel &kernel)
{
    auto g=create(kernel,56003); socket(kernel,g);
    const auto created=kernel.scheduler().create_thread_prepared(g.pid,
        kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}),
        [](sched::ThreadControlBlock &t){return native::prepare_native_thread(t);}); CHECK(created);
    const auto sibling=created.value(); thread(kernel,g,sibling).native_active=true;
    const auto first=arm(kernel,g), second=arm(kernel,g,sibling);
    const auto saved=native::linux_udp_receive_objects.resolve(second); CHECK(saved);
    const auto operation=saved.value()->operation;
    const auto sequence=saved.value()->request_sequence;
    const auto original_ticket=saved.value()->ticket;
    packet(kernel,g); drive(kernel);
    expect(!thread(kernel,g).syscall_pending && !native::linux_udp_receive_objects.resolve(first));
    // Park the completed peer with a real scheduler registration so step selects
    // the second continuation, whose already-ready packet was consumed first.
    select(kernel,g); const auto parked=kernel.scheduler().register_current_wait(sched::ThreadWaitReason::console_read,0); CHECK(parked);
    drive(kernel);
    const auto rearmed=native::linux_udp_receive_objects.resolve(second); CHECK(rearmed);
    expect(rearmed.value()->operation==operation && rearmed.value()->request_sequence==sequence &&
        rearmed.value()->ticket.generation>original_ticket.generation &&
        thread(kernel,g,sibling).wait_registration.pending(rearmed.value()->ticket));
    packet(kernel,g); drive(kernel);
    expect(!thread(kernel,g,sibling).syscall_pending && result(kernel,g,sibling)==3 &&
        !native::linux_udp_receive_objects.resolve(second));
    CHECK(kernel.scheduler().complete_wait(parked.value(),sched::WaitCompletion::cancelled)); release(kernel,g);
}
void original_close_reuse_and_shared_flags(Kernel &kernel)
{
    auto g=create(kernel,56004); socket(kernel,g); const auto object=arm(kernel,g);
    const auto duplicated=kernel.posix().fcntl(g.fd,posix::f_DUPFD,10); CHECK(duplicated);
    const auto duplicate=static_cast<i32>(duplicated.value());
    CHECK(kernel.posix().close(g.fd));
    const auto replacement=kernel.native_udp().open(); CHECK(replacement);
    const auto replaced_fd=kernel.posix().install_native_udp(replacement.value(),false,false); CHECK(replaced_fd);
    expect(replaced_fd.value()==g.fd);
    CHECK(kernel.posix().fcntl(duplicate,posix::f_SETFL,posix::o_NONBLOCK));
    drive(kernel);
    expect(!thread(kernel,g).syscall_pending && result(kernel,g)==error(E::e_again) &&
        !native::linux_udp_receive_objects.resolve(object));
    expect(kernel.scheduler().open_description(g.description)->references==1);
    CHECK(kernel.posix().activate(g.pid)); CHECK(kernel.posix().close(duplicate));
    expect(!kernel.scheduler().open_description(g.description) && !kernel.native_udp().binding(g.endpoint));
    expect(kernel.native_udp().binding(replacement.value()).ok()); release(kernel,g);
    expect(!kernel.native_udp().binding(replacement.value()));
}
void group_and_release_retirement(Kernel &kernel)
{
    for (bool terminated : {false,true})
    {
        auto g=create(kernel,static_cast<u16>(56005+terminated)); socket(kernel,g); const auto object=arm(kernel,g);
        const auto owned=native::linux_udp_receive_objects.resolve(object); CHECK(owned);
        const auto ticket=owned.value()->ticket; const auto lease=thread(kernel,g).entry_stack;
        if (terminated)
        {
            CHECK(native::private_process_kill(kernel,process(kernel,g),g.pid,9)==0);
            expect(!native::step(kernel)); // Fatal cleanup also performs real orphan reaping.
            expect(kernel.scheduler().find(g.pid)==nullptr && kernel.scheduler().find_thread(g.pid,g.pid)==nullptr);
        }
        else
        {
            native::release_process(kernel,g.pid);
            expect(!thread(kernel,g).entry_stack.pool && !process(kernel,g).resources().native_space.root);
            expect(!thread(kernel,g).wait_registration.active() && thread(kernel,g).wait_registration.ticket()==ticket);
            CHECK(kernel.scheduler().exit_process(g.pid,0)); CHECK(kernel.scheduler().reap_orphans());
        }
        expect(native::runtime_status().ok() && !native::linux_udp_receive_objects.resolve(object));
        expect(!native::linux_udp_receive_object(g.pid,g.pid).pool && !native::entry_stacks.top(lease,{g.pid,g.pid}));
        expect(!kernel.native_udp().binding(g.endpoint));
    }
}
void exec_transaction_retirement(Kernel &kernel)
{
    // Genuine replacement engine and scheduler commit, without an ELF loader or
    // guest exec claim. The callback is the transaction's normal retirement API.
    for (bool committed : {false,true})
    {
        auto g=create(kernel,static_cast<u16>(56007+committed)); socket(kernel,g);
        const auto created=kernel.scheduler().create_thread_prepared(g.pid,
            kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}),
            [](sched::ThreadControlBlock &t){return native::prepare_native_thread(t);}); CHECK(created);
        const auto sibling=created.value(); thread(kernel,g,sibling).native_active=true;
        const auto object=arm(kernel,g,sibling); const auto lease=thread(kernel,g,sibling).entry_stack;
        const memory::KernelEntryStackOwner owner{g.pid,sibling}; select(kernel,g);
        bool retired{};
        const auto replaced=native::replace_entry_stacks(native::entry_stacks,process(kernel,g),
            [&]() -> Status {
                if (!committed) return Status::no_memory("hosted precommit scheduler refusal");
                return kernel.scheduler().replace_image(g.pid,
                    kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}),{});
            }, [&]() -> Status {
                CHECK(native::entry_stacks.top(lease,owner));
                const auto status=native::retire_linux_udp_receives_for_exec(kernel,g.pid);
                if (!status) return status;
                expect(!native::linux_udp_receive_objects.resolve(object));
                CHECK(native::entry_stacks.top(lease,owner)); retired=true; return {};
            });
        expect(replaced.committed==committed && retired==committed);
        if (committed)
        {
            expect(static_cast<bool>(replaced) && !native::entry_stacks.top(lease,owner));
            expect(!native::linux_udp_receive_object(g.pid,sibling).pool);
        }
        else
        {
            expect(!replaced && native::linux_udp_receive_object(g.pid,sibling)==object);
            CHECK(native::entry_stacks.top(lease,owner));
            expect(thread(kernel,g,sibling).syscall_pending && thread(kernel,g,sibling).wait_registration.active());
        }
        release(kernel,g);
    }
}

// Provider callback enters the actual group release while a genuine captured
// receive borrow is live. It observes containment before the borrower returns.
Kernel *copy_kernel{};
Group copy_group{};
memory::ObjectHandle copy_object{};
hw::UserFrame copy_frame{};
uptr copy_root{};
void release_during_copy()
{
    mock::after_usercopy=nullptr;
    copy_object=native::linux_udp_receive_object(copy_group.pid,copy_group.pid);
    const auto owned=native::linux_udp_receive_objects.resolve(copy_object); CHECK(owned);
    expect(owned.value()->capture && native::syscall_origins.linux_udp_receive_borrowed(*owned.value()->capture));
    const auto destructions=native::linux_udp_receive_objects.accounting().destructions;
    expect(native::runtime_status().ok());
    native::release_process(*copy_kernel,copy_group.pid);
    expect(native::runtime_status().code()==StatusCode::busy && !native::started);
    expect(native::linux_udp_receive_object(copy_group.pid,copy_group.pid)==copy_object);
    CHECK(native::linux_udp_receive_objects.resolve(copy_object));
    expect(native::linux_udp_receive_objects.accounting().destructions==destructions);
    expect(copy_kernel->scheduler().open_description(copy_group.description)->references==2);
    const auto &t=thread(*copy_kernel,copy_group);
    CHECK(native::entry_stacks.top(t.entry_stack,{copy_group.pid,copy_group.pid}));
    expect(t.native_active && t.syscall_pending &&
        process(*copy_kernel,copy_group).resources().native_space.root==copy_root &&
        native::SyscallOrigins::same_frame(t.native_frame,copy_frame));
}
void first_failure_containment(Kernel &kernel)
{
    // Actual direct owned dispatch and step owned resume exercise the two
    // receive-borrow failure sinks. The synchronous caller guard is statically
    // reviewed; no staged-begin callback or immediate-path borrow is claimed.
    for (unsigned route=0;route<2;++route)
    {
        auto g=create(kernel,static_cast<u16>(56010+route)); socket(kernel,g);
        static_cast<void>(arm(kernel,g)); packet(kernel,g);
        if (route==0) { CHECK(native::scan_linux_udp_receives(kernel)); select(kernel,g); }
        copy_kernel=&kernel; copy_group=g; copy_object={};
        copy_frame=thread(kernel,g).native_frame; copy_root=process(kernel,g).resources().native_space.root;
        const auto lease=thread(kernel,g).entry_stack;
        const auto entries=mock::entries;
        const auto decodes=mock::syscall_decodes;
        mock::after_usercopy=release_during_copy;
        if (route==0)
        {
            bool complete=true;
            const auto value=native::dispatch(kernel,g.pid,hw::Syscall{},complete);
            expect(value==0 && !complete);
        }
        else expect(!native::step(kernel));
        expect(mock::after_usercopy==nullptr && copy_object.pool && mock::entries==entries);
        expect(mock::syscall_decodes==decodes);
        expect(native::runtime_status().code()==StatusCode::busy && !native::started);
        expect(!native::linux_udp_receive_object(g.pid,g.pid).pool &&
            !native::linux_udp_receive_objects.resolve(copy_object));
        expect(kernel.scheduler().open_description(g.description)->references==1);
        CHECK(native::entry_stacks.top(lease,{g.pid,g.pid}));
        expect(thread(kernel,g).native_active && thread(kernel,g).syscall_pending &&
            process(kernel,g).resources().native_space.root==copy_root &&
            native::SyscallOrigins::same_frame(thread(kernel,g).native_frame,copy_frame));
        // Reset only this standalone hosted fixture after checking the terminal
        // failure, then use actual release/reap to retire its retained image.
        native::runtime_failure={}; native::started=true; release(kernel,g);
    }
}
}
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.native_udp().configure(network_link)); CHECK(peer.configure({network_link.peer_mac,network_link.local_mac,network_link.peer_ip,network_link.local_ip}));
    const auto opened=peer.open(); CHECK(opened); sender=opened.value(); CHECK(peer.bind(sender,{},41000));
    plain_nonblock_and_idle(kernel); readiness_stolen(kernel); original_close_reuse_and_shared_flags(kernel);
    group_and_release_retirement(kernel); exec_transaction_retirement(kernel); first_failure_containment(kernel);
    expect(native::entry_stacks.accounting().leased_slots==0 && kernel.native_udp().active_sockets()==0);
    const auto owners=native::linux_udp_receive_objects.accounting();
    expect(owners.live_objects==0 && owners.allocations==owners.destructions && native::linux_udp_receive_start_status().ok());
    CHECK(peer.close(sender));
    std::printf("LINUX_UDP_DISPATCH_INTEGRATION_MODEL_PASS checks=%zu groups=%zu calls=%zu resumes=%zu entries=%zu hosted=1 profile=x86_64 guest=0 actual_nic=0 elf_exec=0\n",
        checks,groups,calls,resumes,mock::entries);
}
