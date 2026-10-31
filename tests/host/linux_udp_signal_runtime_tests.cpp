#include "linux_udp_signal_fixture.hpp"
namespace
{
void publication(Kernel &kernel, unsigned wake, bool shared, bool reused, bool restart_flag)
{
    const auto g=create(kernel); const auto object=arm(kernel,g); caught(kernel,g,10,!restart_flag);
    enqueue(kernel,g,10,shared); caught(kernel,g,10,restart_flag); // Recipient reads the current action.
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    const auto owner=native::linux_udp_receive_objects.resolve(object); CHECK(owner);
    const auto ticket=owner.value()->ticket; const auto binding=*owner.value()->binding;
    const auto frame=t.native_frame; const auto origin=native::syscall_origins.identity(p,t);
    u32 replacement{};
    if(reused)
    {
        const auto actor=kernel.scheduler().create_thread(g.pid,t.context); CHECK(actor);
        CHECK(kernel.scheduler().activate_thread(g.pid,actor.value())); CHECK(kernel.posix().activate(g.pid));
        CHECK(kernel.posix().close(g.fd));
        const auto made=kernel.native_udp().open(); CHECK(made); replacement=made.value();
        const auto fd=kernel.posix().install_native_udp(replacement,false,false); CHECK(fd); expect(fd.value()==g.fd);
        expect(kernel.scheduler().open_description(g.description)->references==1);
    }
    if(wake==1) CHECK(kernel.scheduler().complete_wait(ticket,sched::WaitCompletion::ready));
    if(wake==2)
    {
        packet(kernel,g); CHECK(native::scan_linux_udp_receives(kernel)); CHECK(kernel.native_udp().consume(g.endpoint));
    }
    expect(native::private_linux_udp_signal_ready(kernel,p,t));
    expect(native::private_linux_udp_signal_complete(kernel,p,t));
    expect(!t.syscall_pending && !native::linux_udp_receive_object(g.pid,g.pid).pool);
    expect(!native::linux_udp_receive_objects.resolve(object) && !native::syscall_origins.live(origin));
    expect(t.wait_registration.ticket()==ticket && !t.wait_registration.active());
    expect(t.wait_registration.completion()==(wake ? sched::WaitCompletion::ready : sched::WaitCompletion::interrupted));
    expect(t.native_frame.pc==frame.pc && t.native_frame.registers[14]==static_cast<u64>(-4));
    for(usize index=0;index<t.native_frame.registers.size();++index)
        if(index!=14) expect(t.native_frame.registers[index]==frame.registers[index]);
    expect(t.native_frame.fp_state==frame.fp_state);
    expect(reused ? kernel.scheduler().open_description(g.description)==nullptr :
        kernel.scheduler().open_description(g.description)->references==1);
    expect(!native::private_linux_udp_signal_complete(kernel,p,t));
    CHECK(native::retire_linux_udp_receive(kernel,object,native::LinuxUdpReceiveRetirement::completed));
    expect(native::linux_udp_receive_objects.accounting().live_objects==0);
    expect(binding.object==object && (reused ? kernel.native_udp().binding(replacement).ok() : true));
    saved_result(kernel,g,-4); release(kernel,g);
}
void eligibility(Kernel &kernel, unsigned mode)
{
    const auto g=create(kernel); const auto object=arm(kernel,g); caught(kernel,g); enqueue(kernel,g);
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    const auto frame=t.native_frame; const auto space=p.resources().native_space;
    const auto owner=native::linux_udp_receive_objects.resolve(object); CHECK(owner);
    const auto binding=*owner.value()->binding; const auto original=owner.value()->operation;
    if(mode==0) t.native_signal_mask.set(10);
    if(mode==1) native::private_signal_table(kernel,g.pid)->find(10)->handler=1;
    if(mode==2) native::private_signal_table(kernel,g.pid)->find(10)->flags|=0x400000;
    if(mode==3) ++t.native_frame.registers[7];
    if(mode==4) ++p.resources().native_space.generation;
    if(mode==5) CHECK(kernel.scheduler().complete_wait(binding.ticket,sched::WaitCompletion::cancelled));
    if(mode==6)
    {
        CHECK(native::entry_stacks.activate_entry(t.entry_stack,{g.pid,g.pid},0));
        CHECK(native::entry_stacks.deactivate_entry(t.entry_stack,{g.pid,g.pid},0));
    }
    if(mode==7) ++owner.value()->operation.count;
    if(mode==8) ++owner.value()->binding->request_sequence;
    if(mode==9) native::syscall_origins.revoke(t);
    if(mode==10) owner.value()->phase=native::LinuxUdpReceivePhase::captured_staged;
    if(mode==11)
    {
        CHECK(kernel.scheduler().complete_wait(binding.ticket,sched::WaitCompletion::ready));
        CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
        CHECK(kernel.scheduler().register_current_wait(sched::ThreadWaitReason::deadline,909));
    }
    expect(!native::private_linux_udp_signal_ready(kernel,p,t));
    expect(!native::private_linux_udp_signal_complete(kernel,p,t));
    expect(t.syscall_pending && native::linux_udp_receive_objects.resolve(object).ok());
    expect(native::private_signal_pending(kernel,p,t).pending_mask().contains(10));
    expect(t.native_frame.pc==frame.pc && t.native_frame.registers[14]==frame.registers[14]);
    t.native_frame=frame; p.resources().native_space=space; owner.value()->operation=original;
    owner.value()->binding=binding; owner.value()->phase=native::LinuxUdpReceivePhase::published;
    if(mode==11) owner.value()->ticket=t.wait_registration.ticket(); // Retirement owns the newer exact registration.
    release(kernel,g);
}
void ignored_before_caught(Kernel &kernel)
{
    const auto g=create(kernel); static_cast<void>(arm(kernel,g)); caught(kernel,g); enqueue(kernel,g,10,true);
    native::private_signal_table(kernel,g.pid)->find(4)->handler=1; enqueue(kernel,g,4);
    expect(native::private_linux_udp_signal_complete(kernel,process(kernel,g),thread(kernel,g)));
    saved_result(kernel,g,-4);
    expect(!native::private_signal_pending(kernel,process(kernel,g),thread(kernel,g)).pending_mask().contains(4));
    release(kernel,g);
}
void borrow_and_one_shot(Kernel &kernel)
{
    const auto g=create(kernel); const auto object=arm(kernel,g); caught(kernel,g); enqueue(kernel,g);
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    const auto owner=native::linux_udp_receive_objects.resolve(object); CHECK(owner); auto &wait=*owner.value();
    const auto binding=*wait.binding; CHECK(kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::ready));
    CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
    auto borrowed=native::syscall_origins.acquire_linux_udp_receive_access(*wait.capture,binding); CHECK(borrowed);
    expect(!native::private_linux_udp_signal_ready(kernel,p,t));
    expect(!native::syscall_origins.consume_linux_udp_receive_interruption(*wait.capture,t.native_frame,binding));
    expect(borrowed.value().live() && t.syscall_pending);
    borrowed.value().reset();
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
        expect(!native::syscall_origins.consume_linux_udp_receive_interruption(*wait.capture,t.native_frame,changed));
    }
    auto changed_frame=t.native_frame; ++changed_frame.registers[7];
    expect(!native::syscall_origins.consume_linux_udp_receive_interruption(*wait.capture,changed_frame,binding));
    static native::SyscallOrigins foreign{native::entry_stacks};
    expect(!foreign.consume_linux_udp_receive_interruption(*wait.capture,t.native_frame,binding));
    expect(native::private_linux_udp_signal_ready(kernel,p,t) && t.syscall_pending &&
        t.wait_registration.ticket()==binding.ticket && wait.binding==binding);
    expect(native::syscall_origins.consume_linux_udp_receive_interruption(*wait.capture,t.native_frame,binding).ok());
    expect(!native::syscall_origins.consume_linux_udp_receive_interruption(*wait.capture,t.native_frame,binding));
    expect(!native::syscall_origins.acquire_linux_udp_receive_access(*wait.capture,binding));
    expect(!native::private_linux_udp_signal_complete(kernel,p,t));
    CHECK(native::retire_linux_udp_receive(kernel,object,native::LinuxUdpReceiveRetirement::completed));
    release(kernel,g);
}
void data_and_nonblock_priority(Kernel &kernel, bool nonblock)
{
    const auto g=create(kernel); const auto object=arm(kernel,g); caught(kernel,g); enqueue(kernel,g);
    if(nonblock)
    {
        const auto actor=kernel.scheduler().create_thread(g.pid,thread(kernel,g).context); CHECK(actor);
        CHECK(kernel.scheduler().activate_thread(g.pid,actor.value())); CHECK(kernel.posix().activate(g.pid));
        CHECK(kernel.posix().fcntl(g.fd,posix::f_SETFL,posix::o_NONBLOCK));
    }
    else packet(kernel,g);
    expect(!native::private_linux_udp_signal_ready(kernel,process(kernel,g),thread(kernel,g)));
    const auto entries=mock::entries; native::last_pid=native::last_tid=0;
    expect(native::step(kernel)); expect(mock::entries==entries);
    expect(!thread(kernel,g).syscall_pending && !native::linux_udp_receive_objects.resolve(object));
    const i64 result=nonblock ? -static_cast<i64>(*C::errno_number(lu::UdpErrno::e_again)) : 3;
    expect(static_cast<i64>(thread(kernel,g).native_frame.registers[14])==result);
    expect(native::private_signal_pending(kernel,process(kernel,g),thread(kernel,g)).pending_mask().contains(10));
    saved_result(kernel,g,result); release(kernel,g);
}
void recipient_step(Kernel &kernel, unsigned mode)
{
    const auto g=create(kernel); const auto object=arm(kernel,g); caught(kernel,g); enqueue(kernel,g,10,true);
    const auto ticket=thread(kernel,g).wait_registration.ticket();
    if(mode==1) { packet(kernel,g); CHECK(native::scan_linux_udp_receives(kernel)); CHECK(kernel.native_udp().consume(g.endpoint)); }
    if(mode==2) native::private_signal_table(kernel,g.pid)->find(10)->handler=0;
    mock::entry_trap={hw::TrapKind::timer}; const auto entries=mock::entries;
    native::last_pid=native::last_tid=0; expect(native::step(kernel));
    expect(!native::linux_udp_receive_objects.resolve(object) && !native::linux_udp_receive_object(g.pid,g.pid).pool);
    if(mode==2)
    {
        expect(mock::entries==entries && !thread(kernel,g).native_active);
        CHECK(kernel.scheduler().reap_orphans()); expect(!kernel.scheduler().find(g.pid));
        expect(!kernel.native_udp().binding(g.endpoint));
    }
    else
    {
        auto &t=thread(kernel,g); expect(mock::entries==entries+1 && mock::last_frame.pc==handler && !t.syscall_pending);
        expect(t.wait_registration.ticket()==ticket && !t.wait_registration.active());
        std::array<std::byte,native::X86SignalFrame::prefix_bytes> saved{};
        CHECK(hw::copy_from_user(process(kernel,g).resources().native_space,saved.data(),mock::last_frame.sp,saved.size()));
        expect(native::X86SignalFrame::get(saved,native::X86SignalFrame::context_offset+104,8)==static_cast<u64>(-4));
        expect(native::X86SignalFrame::get(saved,native::X86SignalFrame::context_offset+128,8)==site+2);
        release(kernel,g);
    }
}
template <lu::Profile P> void target_eintr()
{
    hw::UserFrame frame{}; frame.pc=57;
    expect(native::encode_linux_udp_target_result<P>(frame,-4).ok());
    const auto result=lu::NativeBridge<P>::decode_return(frame); CHECK(result);
    expect(result.value().error==4 && result.value().value==0 && frame.pc==57);
}
void terminal_priority(Kernel &kernel)
{
    const auto g=create(kernel); const auto object=arm(kernel,g); caught(kernel,g); enqueue(kernel,g);
    expect(!kernel.poll_network()); // Absent transport retires the actual executor.
    expect(kernel.native_udp_executor().state().failed);
    // A network post after retirement is dropped, not left to keep the loop awake.
    interrupt::post_event(interrupt::EventSource::network);
    expect(!kernel.poll_network() && !interrupt::events_posted());
    expect(!native::private_linux_udp_signal_ready(kernel,process(kernel,g),thread(kernel,g)));
    CHECK(native::scan_linux_udp_receives(kernel)); CHECK(kernel.scheduler().activate_thread(g.pid,g.pid));
    CHECK(native::resume_linux_udp_receive(kernel,process(kernel,g),thread(kernel,g),object));
    expect(static_cast<i64>(thread(kernel,g).native_frame.registers[14])==-static_cast<i64>(*C::errno_number(lu::UdpErrno::e_io)));
    expect(!thread(kernel,g).syscall_pending && !native::linux_udp_receive_objects.resolve(object));
    saved_result(kernel,g,-static_cast<i64>(*C::errno_number(lu::UdpErrno::e_io))); release(kernel,g);
}
void post_consume_failure(Kernel &kernel, std::string_view mode)
{
    const bool publication=mode=="publication" || mode=="combined";
    const bool retirement=mode=="retirement" || mode=="combined";
    expect(publication || retirement);
    const auto g=create(kernel); const auto object=arm(kernel,g); caught(kernel,g); enqueue(kernel,g);
    auto &p=process(kernel,g); auto &t=thread(kernel,g);
    const auto owner=native::linux_udp_receive_objects.resolve(object); CHECK(owner);
    const auto ticket=owner.value()->ticket; const auto origin=native::syscall_origins.identity(p,t);
    const auto before=t.native_frame; const auto destructions=native::linux_udp_receive_objects.accounting().destructions;
    if(publication) native::private_linux_udp_signal_model_result=-4096;
    if(retirement) memory::ObjectPoolTestAccess::short_release_lease(native::linux_udp_receive_objects,object);
    expect(native::private_linux_udp_signal_ready(kernel,p,t));
    expect(!native::private_linux_udp_signal_complete(kernel,p,t));
    expect(!native::started && !native::runtime_status());
    const auto first=native::runtime_status();
    expect(first.code()==StatusCode::invalid_argument);
    expect(first.message()==(publication ? "Linux UDP result is outside the target errno range" :
        "kernel page lease is foreign, modified or stale"));
    expect(!t.syscall_pending && !t.wait_registration.active() && t.wait_registration.ticket()==ticket &&
        t.wait_registration.completion()==sched::WaitCompletion::interrupted);
    expect(!native::syscall_origins.live(origin) && !native::linux_udp_receive_object(g.pid,g.pid).pool &&
        !native::linux_udp_receive_objects.resolve(object));
    expect(kernel.scheduler().open_description(g.description)->references==1);
    const auto accounting=native::linux_udp_receive_objects.accounting();
    expect(accounting.live_objects==0 && accounting.destructions==destructions+1 &&
        accounting.quarantined_pages==static_cast<usize>(retirement));
    expect(t.native_frame.pc==before.pc && t.native_frame.registers[14]==
        (publication ? before.registers[14] : static_cast<u64>(-4)));
    expect(native::private_signal_pending(kernel,p,t).pending_mask().contains(10));
    const auto entries=mock::entries,outputs=mock::outputs;
    expect(!native::private_linux_udp_signal_complete(kernel,p,t));
    expect(!native::step(kernel));
    expect(mock::entries==entries && mock::outputs==outputs && t.native_frame.pc!=handler);
    expect(native::runtime_status().code()==first.code() && native::runtime_status().message()==first.message());
    // These terminal fault processes retain image/stack and quarantined backing;
    // no reset, teardown retry or recovery acceptance is manufactured.
    std::printf("LINUX_UDP_SIGNAL_POST_CONSUME_FAILURE_PASS mode=%.*s checks=%zu publication=%u retirement=%u quarantined=%zu hosted=1 guest=0 actual_nic=0 restart=0\n",
        static_cast<int>(mode.size()),mode.data(),checks,publication,retirement,accounting.quarantined_pages);
}
}
int main(int argc, char **argv)
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(kernel.native_udp().configure(network_link)); CHECK(peer.configure({network_link.peer_mac,network_link.local_mac,network_link.peer_ip,network_link.local_ip}));
    const auto opened=peer.open(); CHECK(opened); sender=opened.value(); CHECK(peer.bind(sender,{},41000));
    if(argc==3 && std::string_view{argv[1]}=="--failure")
    { post_consume_failure(kernel,argv[2]); return 0; }
    expect(argc==1);
    for(unsigned wake=0;wake<3;++wake) for(bool shared : {false,true}) for(bool restart_flag : {false,true})
        publication(kernel,wake,shared,false,restart_flag);
    publication(kernel,0,false,true,false);
    for(unsigned mode=0;mode<12;++mode) eligibility(kernel,mode);
    ignored_before_caught(kernel); borrow_and_one_shot(kernel);
    data_and_nonblock_priority(kernel,false); data_and_nonblock_priority(kernel,true);
    for(unsigned mode=0;mode<3;++mode) recipient_step(kernel,mode);
    target_eintr<lu::Profile::i386>(); target_eintr<lu::Profile::x86_64>(); target_eintr<lu::Profile::aarch64>();
    target_eintr<lu::Profile::arm32>(); target_eintr<lu::Profile::rv64>(); target_eintr<lu::Profile::rv32>();
    target_eintr<lu::Profile::loongarch64>(); target_eintr<lu::Profile::mips>(); target_eintr<lu::Profile::mips64>();
    target_eintr<lu::Profile::ppc>(); terminal_priority(kernel);
    const auto owners=native::linux_udp_receive_objects.accounting();
    expect(owners.live_objects==0 && owners.allocations==owners.destructions);
    expect(native::entry_stacks.accounting().leased_slots==0 && kernel.native_udp().active_sockets()==0);
    CHECK(peer.close(sender));
    std::printf("LINUX_UDP_SIGNAL_RUNTIME_PASS checks=%zu cases=%zu captures=%zu hosted=1 profile=x86_64 guest=0 actual_nic=0 restart=0\n",checks,cases,captures);
}
