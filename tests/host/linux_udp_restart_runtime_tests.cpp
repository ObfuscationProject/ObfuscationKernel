// Hosted actual x86_64 capture/receive/handler/return adapters; synthetic entry.
#include "linux_udp_signal_fixture.hpp"
namespace
{
using Origins = native::SyscallOrigins;
using Binding = Origins::LinuxUdpReceiveBinding;
using Permit = Origins::LinuxUdpRestartPermit;
static_assert(!std::same_as<Permit, Origins::RestartPermit>);
static_assert(!std::same_as<Permit, Origins::LinuxUdpReceiveBorrow>);
void opcode(bool valid = true)
{
    mock::memory[site-hw::user_begin]=std::byte{valid ? u8{0x0f} : u8{0}};
    mock::memory[site-hw::user_begin+1]=std::byte{valid ? u8{0x05} : u8{0}};
}
u64 saved_word(Kernel &kernel, Group g, usize offset)
{
    std::array<std::byte,8> bytes{};
    CHECK(hw::copy_from_user(process(kernel,g).resources().native_space,bytes.data(),
        thread(kernel,g).native_frame.sp+native::X86SignalFrame::context_offset+offset,bytes.size()));
    return native::X86SignalFrame::get(bytes,0,8);
}
void edit_saved_word(Kernel &kernel, Group g, usize offset, u64 value)
{
    std::array<std::byte,8> bytes{}; native::X86SignalFrame::put(bytes,0,value,8);
    CHECK(hw::copy_to_user(process(kernel,g).resources().native_space,
        thread(kernel,g).native_frame.sp+native::X86SignalFrame::context_offset+offset,bytes.data(),bytes.size()));
}
void return_handler(Kernel &kernel, Group g)
{
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
    t.native_frame.sp+=8; // The hosted handler's RET pops the restorer pointer.
    t.native_frame.registers[14]=*lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::rt_sigreturn);
    t.syscall_pending=true;
    bool complete=true; i64 result=918;
    expect(native::private_signal_call(kernel,p,t,complete,result));
    expect(!complete && !t.syscall_pending && t.native_active && native::runtime_status());
    expect(!t.native_signal_mask.contains(10));
}
void observe_restored(Kernel &kernel, Group g)
{
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
    expect(t.native_frame.pc==site && !t.syscall_pending);
    // Synthetic backend executes the restored instruction; capture is genuine
    // shared observation. No saved Plan is supplied to the origin table.
    t.native_frame.pc+=2;
    const auto view=native::entry_stacks.activate_entry(t.entry_stack,{g.pid,g.pid},0); CHECK(view);
    mock::entry_trap={hw::TrapKind::syscall,256};
    const auto observed=native::shared_capture.enter(native::syscall_origins,native::vfs_continuations,
        p,t,view.value(),kernel.scheduler()); CHECK(observed);
    CHECK(native::entry_stacks.deactivate_entry(t.entry_stack,{g.pid,g.pid},0));
    t.syscall_pending=true; ++captures;
    expect(native::dispatch_linux_udp(kernel,p,t).ok());
}
void restart_reexecution(Kernel &kernel, unsigned mode)
{
    const auto g=create(kernel); opcode(); const auto old_object=arm(kernel,g); caught(kernel,g,10,true); enqueue(kernel,g);
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    const auto wait=native::linux_udp_receive_objects.resolve(old_object); CHECK(wait);
    const auto old_binding=*wait.value()->binding;
    const auto old_identity=native::syscall_origins.linux_udp_receive_identity(*wait.value()->capture,t.native_frame,old_binding); CHECK(old_identity);
    const auto copied=native::syscall_origins.prepare_restart(old_identity.value(),t.native_frame,old_binding); CHECK(copied);
    const auto original=t.native_frame;
    if(mode==1)
    { packet(kernel,g); CHECK(native::scan_linux_udp_receives(kernel)); CHECK(kernel.native_udp().consume(g.endpoint)); }
    expect(native::private_linux_udp_signal_complete(kernel,p,t));
    expect(t.native_frame.pc==site && t.native_frame.registers==original.registers && t.native_frame.fp_state==original.fp_state);
    expect(!native::linux_udp_receive_object(g.pid,g.pid).pool && !native::linux_udp_receive_objects.resolve(old_object));
    expect(!t.syscall_pending && !t.wait_registration.active());
    expect(kernel.scheduler().open_description(g.description)->references==1);
    expect(!native::syscall_origins.validate_restart(copied.value(),old_identity.value(),original,old_binding));
    expect(!native::syscall_origins.consume_restart(copied.value(),old_identity.value(),original,old_binding));
    CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
    expect(native::private_signal_deliver(kernel,p,t));
    expect(t.native_frame.pc==handler && saved_word(kernel,g,128)==site &&
        saved_word(kernel,g,104)==*C::syscall_number(lu::UdpCall::recvfrom));
    u32 endpoint=g.endpoint; u16 port=g.port;
    if(mode==2 || mode==3 || mode==4)
    {
        CHECK(kernel.posix().activate(g.pid));
        if(mode==2 || mode==4)
        {
            CHECK(kernel.posix().close(g.fd));
            expect(!kernel.scheduler().open_description(g.description) && !kernel.native_udp().binding(g.endpoint));
        }
        if(mode==2)
        {
            const auto replacement=kernel.native_udp().open(); CHECK(replacement); endpoint=replacement.value(); port+=100;
            CHECK(kernel.native_udp().bind(endpoint,{},port));
            const auto replacement_fd=kernel.posix().install_native_udp(endpoint,false,false); CHECK(replacement_fd);
            expect(replacement_fd.value()==g.fd);
        }
        if(mode==3) CHECK(kernel.posix().fcntl(g.fd,posix::f_SETFL,posix::o_NONBLOCK));
    }
    if(mode==5) { edit_saved_word(kernel,g,72,output+32); edit_saved_word(kernel,g,96,2); }
    return_handler(kernel,g);
    expect(t.native_frame.pc==site && t.native_frame.registers[14]==*C::syscall_number(lu::UdpCall::recvfrom));
    expect(!native::syscall_origins.consume_restart(copied.value(),old_identity.value(),t.native_frame,old_binding));
    observe_restored(kernel,g);
    if(mode==3 || mode==4)
    {
        expect(!t.syscall_pending && !native::linux_udp_receive_object(g.pid,g.pid).pool);
        expect(static_cast<i64>(t.native_frame.registers[14])==(mode==3 ? -11 : -9));
    }
    else
    {
        const auto new_object=native::linux_udp_receive_object(g.pid,g.pid); expect(new_object.pool && new_object!=old_object);
        const auto next=native::linux_udp_receive_objects.resolve(new_object); CHECK(next);
        const auto next_binding=*next.value()->binding;
        const auto next_identity=native::syscall_origins.linux_udp_receive_identity(*next.value()->capture,t.native_frame,next_binding); CHECK(next_identity);
        expect(next_identity.value().capture>old_identity.value().capture && next_identity.value().epoch>old_identity.value().epoch);
        expect(next_binding.ticket.generation>old_binding.ticket.generation && next_binding.endpoint==endpoint);
        expect(!native::syscall_origins.validate_restart(copied.value(),old_identity.value(),t.native_frame,next_binding));
        if(mode==5) expect(next_binding.operation.payload==output+32 && next_binding.operation.count==2);
        packet(kernel,Group{g.pid,g.fd,0,port,endpoint}); CHECK(native::scan_linux_udp_receives(kernel));
        CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
        CHECK(native::resume_linux_udp_receive(kernel,p,t,new_object));
        expect(static_cast<i64>(t.native_frame.registers[14])==(mode==5 ? 2 : 3));
        expect(!t.syscall_pending && !native::linux_udp_receive_object(g.pid,g.pid).pool);
        expect(!native::linux_udp_receive_objects.resolve(new_object));
    }
    release(kernel,g);
}
void permit_denials(Kernel &kernel)
{
    const auto g=create(kernel); opcode(); const auto object=arm(kernel,g); caught(kernel,g,10,true); enqueue(kernel,g);
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    const auto wait=native::linux_udp_receive_objects.resolve(object); CHECK(wait); const auto binding=*wait.value()->binding;
    const auto identity=native::syscall_origins.linux_udp_receive_identity(*wait.value()->capture,t.native_frame,binding); CHECK(identity);
    const auto permit=native::syscall_origins.prepare_restart(identity.value(),t.native_frame,binding); CHECK(permit);
    expect(!native::syscall_origins.identity(p,t).capture && !native::syscall_origins.live(identity.value()));
    static Origins foreign{native::entry_stacks};
    expect(!foreign.validate_restart(permit.value(),identity.value(),t.native_frame,binding));
    expect(!native::syscall_origins.validate_restart(Permit{},identity.value(),t.native_frame,binding));
    for(unsigned mutation=0;mutation<8;++mutation)
    {
        auto changed=binding;
        if(mutation==0) ++changed.object.generation;
        if(mutation==1) ++changed.ticket.generation;
        if(mutation==2) ++changed.request_sequence;
        if(mutation==3) ++changed.description;
        if(mutation==4) ++changed.endpoint;
        if(mutation==5) ++changed.operation.count;
        if(mutation==6) changed.object.pool=nullptr;
        if(mutation==7) changed.ticket={};
        expect(!native::syscall_origins.prepare_restart(identity.value(),t.native_frame,changed));
        expect(!native::syscall_origins.validate_restart(permit.value(),identity.value(),t.native_frame,changed));
        expect(!native::syscall_origins.linux_udp_receive_restart_qualified(*wait.value()->capture,t.native_frame,changed));
    }
    for(unsigned mutation=0;mutation<7;++mutation)
    {
        auto changed=identity.value();
        if(mutation==0) ++changed.capture;
        if(mutation==1) ++changed.image;
        if(mutation==2) ++changed.epoch;
        if(mutation==3) ++changed.space.generation;
        if(mutation==4) ++changed.lease.generation;
        if(mutation==5) ++changed.tid;
        if(mutation==6) ++changed.lower;
        expect(!native::syscall_origins.validate_restart(permit.value(),changed,t.native_frame,binding));
    }
    CHECK(kernel.scheduler().complete_wait(binding.ticket,sched::WaitCompletion::ready));
    CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
    auto borrow=native::syscall_origins.acquire_linux_udp_receive_access(*wait.value()->capture,binding); CHECK(borrow);
    expect(!native::syscall_origins.prepare_restart(identity.value(),t.native_frame,binding));
    expect(!native::syscall_origins.linux_udp_receive_restart_qualified(*wait.value()->capture,t.native_frame,binding));
    expect(!native::private_linux_udp_signal_complete(kernel,p,t)); borrow.value().reset();
    expect(native::syscall_origins.validate_restart(permit.value(),identity.value(),t.native_frame,binding));
    // Mutable code cannot replace the captured opcode. Reexecution captures it
    // anew; this old permit remains an immutable original-input proof.
    opcode(false); expect(native::syscall_origins.validate_restart(permit.value(),identity.value(),t.native_frame,binding));
    expect(native::private_linux_udp_signal_complete(kernel,p,t)); release(kernel,g);
}
void opcode_independent_eintr(Kernel &kernel, bool qualified)
{
    const auto g=create(kernel); opcode(qualified); const auto object=arm(kernel,g);
    caught(kernel,g,10,!qualified); enqueue(kernel,g);
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    const auto wait=native::linux_udp_receive_objects.resolve(object); CHECK(wait);
    const auto qualification=native::syscall_origins.linux_udp_receive_restart_qualified(
        *wait.value()->capture,t.native_frame,*wait.value()->binding); CHECK(qualification);
    expect(qualification.value()==qualified);
    expect(native::private_linux_udp_signal_complete(kernel,p,t));
    expect(t.native_frame.pc==site+2 && static_cast<i64>(t.native_frame.registers[14])==-4);
    saved_result(kernel,g,-4); release(kernel,g);
}
void image_replacement(Kernel &kernel)
{
    const auto g=create(kernel); opcode(); const auto object=arm(kernel,g); caught(kernel,g,10,true); enqueue(kernel,g);
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    const auto wait=native::linux_udp_receive_objects.resolve(object); CHECK(wait); const auto binding=*wait.value()->binding;
    const auto identity=native::syscall_origins.linux_udp_receive_identity(*wait.value()->capture,t.native_frame,binding); CHECK(identity);
    const auto permit=native::syscall_origins.prepare_restart(identity.value(),t.native_frame,binding); CHECK(permit);
    const auto old_space=p.resources().native_space; ++p.resources().native_space.generation;
    expect(!native::syscall_origins.validate_restart(permit.value(),identity.value(),t.native_frame,binding));
    expect(!native::syscall_origins.linux_udp_receive_restart_qualified(*wait.value()->capture,t.native_frame,binding));
    expect(!native::private_linux_udp_signal_complete(kernel,p,t));
    p.resources().native_space=old_space;
    CHECK(native::retire_linux_udp_receives(kernel,g.pid,{},native::LinuxUdpReceiveRetirement::exec));
    CHECK(native::syscall_origins.prepare(t));
    expect(!native::syscall_origins.validate_restart(permit.value(),identity.value(),t.native_frame,binding));
    release(kernel,g);
}
void restart_retirement_failure(Kernel &kernel)
{
    const auto g=create(kernel); opcode(); const auto object=arm(kernel,g); caught(kernel,g,10,true); enqueue(kernel,g);
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    const auto wait=native::linux_udp_receive_objects.resolve(object); CHECK(wait); const auto binding=*wait.value()->binding;
    const auto identity=native::syscall_origins.linux_udp_receive_identity(*wait.value()->capture,t.native_frame,binding); CHECK(identity);
    const auto permit=native::syscall_origins.prepare_restart(identity.value(),t.native_frame,binding); CHECK(permit);
    memory::ObjectPoolTestAccess::short_release_lease(native::linux_udp_receive_objects,object);
    expect(!native::private_linux_udp_signal_complete(kernel,p,t));
    const auto failure=native::runtime_status(); expect(!failure && !native::started);
    expect(failure.message()=="kernel page lease is foreign, modified or stale");
    expect(t.native_frame.pc==site && !t.syscall_pending && !t.wait_registration.active());
    expect(!native::syscall_origins.validate_restart(permit.value(),identity.value(),t.native_frame,binding));
    expect(!native::linux_udp_receive_object(g.pid,g.pid).pool && !native::linux_udp_receive_objects.resolve(object));
    expect(kernel.scheduler().open_description(g.description)->references==1);
    expect(native::linux_udp_receive_objects.accounting().quarantined_pages==1);
    const auto entries=mock::entries, writes=mock::outputs;
    expect(!native::private_linux_udp_signal_complete(kernel,p,t)); expect(!native::step(kernel));
    expect(mock::entries==entries && mock::outputs==writes && t.native_frame.pc!=handler);
    expect(native::runtime_status().message()==failure.message());
    std::printf("LINUX_UDP_RESTART_RETIREMENT_FAILURE_PASS checks=%zu quarantined=1 hosted=1 profile=x86_64 guest=0 actual_nic=0\n",checks);
}
}
int main(int argc, char **argv)
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.native_udp().configure(network_link)); CHECK(peer.configure({network_link.peer_mac,network_link.local_mac,network_link.peer_ip,network_link.local_ip}));
    const auto opened=peer.open(); CHECK(opened); sender=opened.value(); CHECK(peer.bind(sender,{},41000));
    if(argc==2 && std::string_view{argv[1]}=="--retirement-failure") { restart_retirement_failure(kernel); return 0; }
    expect(argc==1);
    for(unsigned mode=0;mode<6;++mode) restart_reexecution(kernel,mode);
    permit_denials(kernel); image_replacement(kernel);
    opcode_independent_eintr(kernel,false); opcode_independent_eintr(kernel,true);
    expect(native::linux_udp_receive_objects.accounting().live_objects==0);
    expect(native::entry_stacks.accounting().leased_slots==0 && kernel.native_udp().active_sockets()==0);
    CHECK(peer.close(sender));
    std::printf("LINUX_UDP_RESTART_RUNTIME_PASS checks=%zu cases=%zu captures=%zu hosted=1 profile=x86_64 guest=0 actual_nic=0\n",checks,cases,captures);
}
