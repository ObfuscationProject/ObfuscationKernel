// Actual private x86 runtime/scheduler with synthetic trap and user-copy backends.
#define OK_ENABLE_TEST_POINTS 1
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_CHILD_WAIT_SIGNAL_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>
namespace
{
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "CHILD_WAIT_SIGNAL_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr uptr output=hw::user_begin+128, site=hw::user_begin+1024, handler=hw::user_begin+2048;
constexpr u64 wait_number=*lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::wait4);
struct Group { sched::ProcessId pid{}; sched::ThreadId waiter{}; sched::ProcessId child{}; };
Group create(Kernel &kernel)
{
    auto made=kernel.scheduler().create_user_process("child-signal-model",kernel.arch().make_user_context({hw::user_begin,hw::user_begin+8192}));CHECK(made);
    auto *p=kernel.scheduler().find(made.value());p->resources().personality=sched::UserPersonality::linux;
    p->resources().native_space={0x100000+made.value()*4096,static_cast<u32>(made.value()%16),1};
    CHECK(native::allocate(p->resources().native_pages,hw::user_begin,hw::page_read|hw::page_write|hw::page_execute));
    auto peer=kernel.scheduler().create_thread(p->pid(),p->threads()[0].context);CHECK(peer);
    for(auto &t:p->threads()){CHECK(native::prepare_native_thread(t));t.native_active=true;}
    p->threads()[0].native_active=false;CHECK(kernel.scheduler().set_runnable(p->pid()));CHECK(kernel.posix().activate(p->pid()));
    auto child=kernel.posix().fork(p->pid());CHECK(child);
    native::runtime_kernel=&kernel;native::started=true;
    return{made.value(),peer.value(),child.value()};
}
void activate(Kernel &kernel,Group g)
{ CHECK(kernel.posix().activate(g.pid));CHECK(kernel.scheduler().activate_thread(g.pid,g.waiter)); }
void retire(Kernel &kernel,Group g)
{
    mock::forbidden=mock::write_forbidden=mock::partial_output=0;
    if(auto *p=kernel.scheduler().find(g.pid))
    {
        const auto children=p->children();
        for(auto id:children)if(auto *c=kernel.scheduler().find(id);c&&c->state()!=sched::ProcessState::zombie)
            CHECK(kernel.scheduler().exit_process(id,0));
        native::release_process(kernel,g.pid);
        if(kernel.scheduler().find(g.pid)->state()!=sched::ProcessState::zombie)CHECK(kernel.scheduler().exit_process(g.pid,0));
    }
    CHECK(kernel.scheduler().reap_orphans());
    expect(native::child_wait_objects.accounting().live_objects==0);expect(native::runtime_status().ok());
}
void frame(Kernel &kernel,Group g,i32 selector,bool reset=true)
{
    auto &f=kernel.scheduler().find_thread(g.pid,g.waiter)->native_frame;
    if(reset){f={};f.flags=0x202;f.sp=hw::user_begin+8192;
        const auto fp=native::X86SignalFrame::reset_fp();for(usize i=0;i<fp.size();++i)f.fp_state[i]=static_cast<u8>(fp[i]);}
    f.pc=site+2;f.registers[14]=wait_number;f.registers[9]=static_cast<u64>(static_cast<i64>(selector));
    f.registers[10]=output;f.registers[11]=0;f.registers[5]=0;
    mock::memory[site-hw::user_begin]=std::byte{0x0f};mock::memory[site-hw::user_begin+1]=std::byte{0x05};
}
void arm(Kernel &kernel,Group g,i32 selector,bool reset=true)
{
    frame(kernel,g,selector,reset);mock::entry_trap={hw::TrapKind::syscall,256};native::last_pid=native::last_tid=0;
    expect(native::step(kernel));auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    expect(t->syscall_pending&&t->wait_reason==sched::ThreadWaitReason::child);
    expect(native::syscall_origins.live(native::syscall_origins.identity(*kernel.scheduler().find(g.pid),*t)));
    expect(native::child_wait_object(g.pid,g.waiter).pool!=nullptr);
}
void caught(Kernel &kernel,Group g,bool restart,u32 signal=10)
{
    auto *a=native::private_signal_table(kernel,g.pid)->find(signal);CHECK(a);
    a->handler=handler;a->restorer=handler+4;a->flags=0x04000004|(restart?0x10000000:0);
}
void enqueue(Kernel &kernel,Group g,u32 signal=10)
{
    auto *q=native::private_signal_queue(*kernel.scheduler().find_thread(g.pid,g.waiter));CHECK(q);
    expect(q->enqueue({signal,-6,static_cast<u32>(g.pid),0})==native::PrivateSignalQueue::Enqueue::queued);
}
u32 get32(uptr address,bool big=false)
{
    u32 value{};for(usize i=0;i<4;++i)value|=std::to_integer<u32>(mock::memory[address-hw::user_begin+i])<<(8*(big?3-i:i));return value;
}
void sigreturn(Kernel &kernel,Group g,hw::UserFrame saved)
{
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    t->native_frame=saved;t->native_frame.sp+=8;t->native_frame.registers[14]=15;
    bool complete=true;i64 result=-999;expect(native::private_signal_call(kernel,*p,*t,complete,result));expect(!complete&&result==0);
}
void publication(Kernel &kernel,bool restart,bool ready,bool any)
{
    const auto g=create(kernel);arm(kernel,g,any?-1:static_cast<i32>(g.child));caught(kernel,g,!restart);enqueue(kernel,g);caught(kernel,g,restart);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    const auto object=native::child_wait_object(g.pid,g.waiter);const auto wait=*native::child_wait_objects.resolve(object).value();
    const auto origin=native::syscall_origins.identity(*p,*t);const auto before=t->native_frame;
    expect(native::private_signal_admit_blocked(kernel,*p,*t));
    if(ready)CHECK(kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::ready));
    const auto outputs=mock::outputs;
    expect(native::private_child_signal_ready(kernel,*p,*t));expect(native::private_child_signal_complete(kernel,*p,*t));
    expect(mock::outputs==outputs);expect(!t->syscall_pending&&!native::child_wait_object(g.pid,g.waiter).pool);
    expect(!native::syscall_origins.live(origin)&&!native::child_wait_objects.resolve(object));
    expect(t->wait_registration.completion()==(ready?sched::WaitCompletion::ready:sched::WaitCompletion::interrupted));
    expect(t->native_frame.pc==(restart?site:site+2));expect(t->native_frame.registers[14]==(restart?wait_number:static_cast<u64>(-4)));
    for(usize i:{9,10,11,5})expect(t->native_frame.registers[i]==before.registers[i]);
    expect(!native::private_child_signal_complete(kernel,*p,*t));expect(native::private_signal_deliver(kernel,*p,*t));
    const auto outer=t->native_frame;expect(outer.pc==handler);sigreturn(kernel,g,outer);
    expect(t->native_frame.pc==(restart?site:site+2));expect(!native::child_wait_object(g.pid,g.waiter).pool);
    if(restart){arm(kernel,g,any?-1:static_cast<i32>(g.child),false);const auto next=*native::child_wait_objects.resolve(native::child_wait_object(g.pid,g.waiter)).value();expect(next.ticket.generation>wait.ticket.generation);}
    retire(kernel,g);
}
void eligibility(Kernel &kernel,unsigned mode)
{
    auto g=create(kernel);arm(kernel,g,static_cast<i32>(g.child));caught(kernel,g,true);enqueue(kernel,g);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    const auto object=native::child_wait_object(g.pid,g.waiter);auto &wait=*native::child_wait_objects.resolve(object).value();const auto before=t->native_frame;
    if(mode==0)t->native_signal_mask.set(10);
    if(mode==1)native::private_signal_table(kernel,g.pid)->find(10)->handler=1;
    if(mode==2)native::syscall_origins.revoke(*t);
    if(mode==3)++t->native_frame.registers[7];
    if(mode==4)CHECK(kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::cancelled));
    if(mode==5){CHECK(native::entry_stacks.activate_entry(t->entry_stack,{g.pid,g.waiter},0));CHECK(native::entry_stacks.deactivate_entry(t->entry_stack,{g.pid,g.waiter},0));}
    if(mode==6){CHECK(kernel.scheduler().complete_wait(wait.ticket,sched::WaitCompletion::ready));activate(kernel,g);CHECK(kernel.scheduler().register_current_wait(sched::ThreadWaitReason::deadline,909));}
    if(mode==7)++wait.operation.output;
    if(mode==8)++wait.request;
    if(mode==9)mock::write_forbidden=output;
    if(mode==10)CHECK(kernel.scheduler().exit_process(g.child,45));
    if(mode==11){CHECK(kernel.scheduler().exit_process(g.child,45));CHECK(kernel.scheduler().wait_process(g.pid,g.child));p=kernel.scheduler().find(g.pid);t=kernel.scheduler().find_thread(g.pid,g.waiter);}
    expect(!native::private_child_signal_ready(kernel,*p,*t));expect(!native::private_child_signal_complete(kernel,*p,*t));
    expect(native::child_wait_objects.resolve(object).ok()&&t->syscall_pending);
    expect(t->native_frame.pc==before.pc&&t->native_frame.registers[14]==before.registers[14]);
    expect(native::private_signal_queue(*t)->pending_mask().contains(10));
    if(mode==6){const auto replacement=t->wait_registration.ticket();native::cancel_child_wait(kernel,object);expect(t->wait_registration.pending(replacement));}
    retire(kernel,g);
}
void precedence(Kernel &kernel,unsigned mode)
{
    auto g=create(kernel);arm(kernel,g,static_cast<i32>(g.child));caught(kernel,g,true);enqueue(kernel,g);
    if(mode==0)CHECK(kernel.scheduler().exit_process(g.child,37));
    if(mode==1){CHECK(kernel.scheduler().exit_process(g.child,37));CHECK(kernel.scheduler().wait_process(g.pid,g.child));}
    if(mode==2)mock::write_forbidden=output;
    if(mode==3){CHECK(kernel.scheduler().terminate_process(g.child,9));mock::partial_output=1;}
    mock::entry_trap={hw::TrapKind::timer};expect(native::step(kernel));
    auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);expect(!t->syscall_pending&&!native::child_wait_object(g.pid,g.waiter).pool);
    const i64 expected=mode==0?static_cast<i64>(g.child):mode==1?-10:-14;
    expect(t->native_frame.registers[14]==static_cast<u64>(expected));
    expect(native::private_signal_queue(*t)->pending_mask().contains(10));
    if(mode==0)expect(get32(output)==(37U<<8));
    if(mode<2||mode==3)expect(!kernel.scheduler().find(g.child));
    retire(kernel,g);
}
void peer_rearm(Kernel &kernel,bool signal)
{
    auto g=create(kernel);const auto other=kernel.posix().fork(g.pid);CHECK(other);arm(kernel,g,-1);
    const auto object=native::child_wait_object(g.pid,g.waiter);const auto old=*native::child_wait_objects.resolve(object).value();
    CHECK(kernel.scheduler().exit_process(g.child,7));CHECK(kernel.scheduler().wait_process(g.pid,g.child));
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    const auto origin=native::syscall_origins.identity(*p,*t);caught(kernel,g,true);
    if(signal){enqueue(kernel,g);expect(native::private_child_signal_complete(kernel,*p,*t));}
    else{
        activate(kernel,g);bool complete=true;expect(native::resume_child_wait(kernel,*p,object,complete)==-11&&!complete);
        const auto current=*native::child_wait_objects.resolve(object).value();expect(current.ticket.generation>old.ticket.generation&&current.request==old.request);
        expect(native::syscall_origins.validate_bound(origin,t->native_frame,native::child_origin_binding(object,current)));
        expect(!native::syscall_origins.validate_bound(origin,t->native_frame,native::child_origin_binding(object,old)));
        CHECK(kernel.scheduler().exit_process(other.value(),23));mock::entry_trap={hw::TrapKind::timer};expect(native::step(kernel));
        expect(kernel.scheduler().find_thread(g.pid,g.waiter)->native_frame.registers[14]==other.value());expect(get32(output)==23U<<8);
    }
    retire(kernel,g);
}
void nested(Kernel &kernel)
{
    auto g=create(kernel);arm(kernel,g,-1);caught(kernel,g,true);enqueue(kernel,g);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    expect(native::private_child_signal_complete(kernel,*p,*t));expect(native::private_signal_deliver(kernel,*p,*t));const auto outer=t->native_frame;
    arm(kernel,g,-1,false);caught(kernel,g,false,12);enqueue(kernel,g,12);
    expect(native::private_child_signal_complete(kernel,*p,*t));expect(native::private_signal_deliver(kernel,*p,*t));const auto inner=t->native_frame;
    expect(inner.sp<outer.sp);sigreturn(kernel,g,inner);expect(t->native_frame.pc==site+2&&t->native_frame.registers[14]==static_cast<u64>(-4));
    expect(t->native_signal_mask.contains(10)&&!t->native_signal_mask.contains(12));sigreturn(kernel,g,outer);
    expect(t->native_frame.pc==site&&t->native_frame.registers[14]==wait_number);expect(!t->native_signal_mask.contains(10));retire(kernel,g);
}
void teardown(Kernel &kernel,unsigned mode)
{
    auto g=create(kernel);arm(kernel,g,-1);auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    const auto object=native::child_wait_object(g.pid,g.waiter);const auto origin=native::syscall_origins.identity(*p,*t);
    if(mode==0){native::ExecRetirement retirement(*p,g.pid);retirement.consume(kernel,*p);}
    if(mode==1){caught(kernel,g,true);native::private_signal_table(kernel,g.pid)->find(10)->handler=0;enqueue(kernel,g);expect(!native::private_child_signal_complete(kernel,*p,*t));}
    if(mode==2){CHECK(kernel.scheduler().complete_wait(t->wait_registration.ticket(),sched::WaitCompletion::cancelled));activate(kernel,g);bool done=true;expect(native::dispatch(kernel,g.pid,{OK_SYS_EXIT,{}},done)==-125);expect(native::dispatch(kernel,g.pid,{OK_SYS_EXIT,{}},done)==0);}
    if(mode==3){native::release_process(kernel,g.pid);}
    if(mode==4){caught(kernel,g,true);enqueue(kernel,g);expect(native::private_child_signal_complete(kernel,*p,*t));mock::write_forbidden=t->native_frame.sp-512;expect(!native::private_signal_deliver(kernel,*p,*t));mock::write_forbidden=0;}
    expect(!native::child_wait_objects.resolve(object)&&!native::syscall_origins.live(origin));retire(kernel,g);
}
void sender(Kernel &kernel)
{
    auto g=create(kernel);arm(kernel,g,-1);caught(kernel,g,false);auto *p=kernel.scheduler().find(g.pid);
    auto *from=kernel.scheduler().find_thread(g.pid,g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    from->native_frame.registers[14]=200;from->native_frame.registers[9]=g.waiter;from->native_frame.registers[10]=10;
    bool done=true;i64 result=-999;expect(native::private_signal_call(kernel,*p,*from,done,result)&&result==0);
    expect(t->wait_registration.active()&&t->syscall_pending);expect(native::private_signal_queue(*t)->pending_mask().contains(10));
    mock::entry_trap={hw::TrapKind::timer};expect(native::step(kernel));expect(mock::last_frame.pc==handler&&!t->syscall_pending);retire(kernel,g);
}
void ignored_and_late(Kernel &kernel)
{
    auto g=create(kernel);arm(kernel,g,-1);caught(kernel,g,false);
    auto *p=kernel.scheduler().find(g.pid);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);
    native::private_signal_table(kernel,g.pid)->find(4)->handler=1;enqueue(kernel,g,4);enqueue(kernel,g);
    expect(native::private_child_signal_complete(kernel,*p,*t));expect(native::private_signal_deliver(kernel,*p,*t));
    expect(t->native_frame.pc==handler&&!native::private_signal_queue(*t)->pending_mask().contains(4));retire(kernel,g);
    g=create(kernel);caught(kernel,g,true);frame(kernel,g,-1);
    // Signal arrival after owned capture and before syscall registration.
    mock::owned_hook=[](hw::AddressSpace &,hw::UserFrame &,const hw::KernelEntryStackView &){
        auto *current=native::runtime_kernel->scheduler().current_thread();CHECK(current);
        auto *q=native::private_signal_queue(*current);CHECK(q);CHECK(q->enqueue({10,-6,static_cast<u32>(current->owner),0})==native::PrivateSignalQueue::Enqueue::queued);
    };
    mock::entry_trap={hw::TrapKind::syscall,256};native::last_pid=native::last_tid=0;expect(native::step(kernel));mock::owned_hook=nullptr;
    t=kernel.scheduler().find_thread(g.pid,g.waiter);expect(t->syscall_pending&&native::child_wait_object(g.pid,g.waiter).pool);
    mock::entry_trap={hw::TrapKind::timer};expect(native::step(kernel));expect(mock::last_frame.pc==handler&&!t->syscall_pending);retire(kernel,g);
}
template<lu::Profile P> void adapters(Kernel &kernel,usize number,std::array<int,6> slots)
{
    using Codec=lu::Codec<P>;using Origin=native::SyscallOriginTable<P>;using Restart=native::RestartAdapter<P>;
    constexpr bool siginfo=P==lu::Profile::rv32;
    const auto g=create(kernel);
    auto setup=[&](i32 selector,u32 options,uptr destination=output){
        hw::UserFrame f{};f.sp=hw::user_begin+7168;f.flags=0x10;f.pc=site+4;
        f.registers[number]=*Codec::syscall_number(siginfo?lu::Operation::waitid:lu::Operation::wait4);
        std::array<u64,6>a{};
        if constexpr(siginfo)a={selector==-1?0U:1U,selector==-1?0U:static_cast<u32>(selector),destination,4U|options,0,0};
        else a={static_cast<u32>(selector),destination,options,0,0,0};
        for(usize i=0;i<a.size();++i)if(slots[i]>=0)f.registers[slots[i]]=a[i];
        if constexpr(Codec::profile.word_bytes==8){f.registers[slots[0]]|=u64{1}<<32;f.registers[slots[2]]|=u64{1}<<32;}
        if constexpr(P==lu::Profile::arm32)f.registers[15]=f.pc;
        return f;
    };
    auto call=[&](hw::UserFrame &f,bool &complete){activate(kernel,g);complete=true;
        return user::linux_bootstrap::dispatch<P>(kernel,g.pid,f,complete,[](hw::Syscall,bool &)->i64{std::abort();});};
    bool complete=true;auto f=setup(g.child,1);mock::memory.fill(std::byte{0x5a});
    expect(call(f,complete)==0&&complete);expect(!native::child_wait_object(g.pid,g.waiter).pool);
    for(usize i=0;i<(siginfo?128:4);++i)expect(mock::memory[output-hw::user_begin+i]==(siginfo?std::byte{}:std::byte{0x5a}));
    f=setup(g.child,2);expect(call(f,complete)==-22&&complete);
    f=setup(g.child,0,hw::user_begin+8191);expect(call(f,complete)==-14&&complete);expect(kernel.scheduler().find(g.child)!=nullptr);
    f=setup(g.child,0);auto *t=kernel.scheduler().find_thread(g.pid,g.waiter);t->syscall_pending=true;
    expect(call(f,complete)==-11&&!complete);const auto object=native::child_wait_object(g.pid,g.waiter);
    const auto copied=native::child_wait_objects.resolve(object).value()->operation;
    expect(copied.selector==static_cast<i32>(g.child)&&copied.output==output&&copied.siginfo==siginfo&&copied.big_endian==Codec::profile.big_endian);
    auto *child=kernel.scheduler().find(g.child);auto cred=child->credentials();cred.uid=9876;child->set_credentials(cred);CHECK(kernel.scheduler().exit_process(g.child,63));
    // Completion must use the copied destination and fresh child UID, without
    // fetching a status word from the user mapping. Adapter request is changed.
    const auto reads=mock::copies;f=setup(-1,1,output+512);expect(call(f,complete)==(siginfo?0:static_cast<i64>(g.child))&&complete);
    expect(mock::copies==reads);expect(!kernel.scheduler().find(g.child));
    expect(get32(output+(siginfo?20:0),Codec::profile.big_endian)==(siginfo?63U:63U<<8));
    if constexpr(siginfo){expect(get32(output)==17);expect(get32(output+12)==g.child);expect(get32(output+16)==9876);}
    CHECK(user::linux_bootstrap::complete<P>(f,-4));auto returned=lu::NativeBridge<P>::decode_return(f);expect(returned&&returned.value().error==4);
    retire(kernel,g);
    for(bool any:{false,true})
    {
        const auto group=create(kernel);auto *p=kernel.scheduler().find(group.pid);auto *thread=kernel.scheduler().find_thread(group.pid,group.waiter);
        // This is an all-profile typed origin model using the actual entry pool
        // and scheduler. No architecture's instructions are executed here.
        static Origin origins(native::entry_stacks);CHECK(origins.prepare(*thread));
        auto capture=setup(any?-1:static_cast<i32>(group.child),0); // setup has no process-dependent state.
        std::array<u8,4> opcode{};usize bytes=4;hw::Trap trap{hw::TrapKind::syscall,8};
        if constexpr(P==lu::Profile::x86_64){opcode={0x0f,0x05};bytes=2;trap.vector=256;}
        else if constexpr(P==lu::Profile::i386){opcode={0xcd,0x80};bytes=2;trap.vector=128;}
        else if constexpr(P==lu::Profile::aarch64){opcode={1,0,0,0xd4};trap.vector=0x15;trap.error_code=0x56000000;}
        else if constexpr(P==lu::Profile::arm32){opcode={0,0,0,0xef};trap.vector=2;}
        else if constexpr(P==lu::Profile::rv32||P==lu::Profile::rv64){opcode={0x73,0,0,0};trap.error_code=8;}
        else if constexpr(P==lu::Profile::loongarch64){opcode={0,0,0x2b,0};trap.vector=11;}
        else if constexpr(P==lu::Profile::mips||P==lu::Profile::mips64){opcode={0,0,0,12};trap.error_code=32;}
        else opcode={0x44,0,0,2};
        capture.pc=site+bytes;if constexpr(P==lu::Profile::arm32)capture.registers[15]=capture.pc;
        thread->native_frame=capture;thread->syscall_pending=false;mock::entry_trap=trap;
        for(usize i=0;i<bytes;++i)mock::memory[site-hw::user_begin+i]=std::byte{opcode[i]};
        auto active=native::entry_stacks.activate_entry(thread->entry_stack,{group.pid,group.waiter},0);CHECK(active);
        CHECK(origins.enter(*p,*thread,active.value(),kernel.scheduler()));CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack,{group.pid,group.waiter},0));
        auto identity=origins.identity(*p,*thread);expect(identity.capture!=0);expect(Restart::family(capture)==Restart::Family::child_wait);
        activate(kernel,group);thread->syscall_pending=true;auto ticket=kernel.scheduler().register_current_wait(sched::ThreadWaitReason::child,any?0:group.child);CHECK(ticket);
        typename Origin::ChildBinding binding{{&origins,1,1,1},ticket.value(),ticket.value().generation,{any?-1:static_cast<i32>(group.child),output,false,siginfo,Codec::profile.big_endian}};
        auto bad=binding;++bad.operation.output;expect(!origins.bind(identity,capture,bad));expect(origins.bind(identity,capture,binding));
        expect(!origins.validate_bound(identity,capture,typename Origin::PipeBinding{}));
        for(unsigned mutation=0;mutation<9;++mutation)
        {
            bad=binding;switch(mutation){case 0:++bad.operation.output;break;case 1:++bad.operation.selector;break;case 2:bad.operation.siginfo=!siginfo;break;case 3:bad.operation.big_endian=!Codec::profile.big_endian;break;case 4:bad.operation.nohang=true;break;case 5:++bad.ticket.generation;break;case 6:++bad.object.generation;break;case 7:++bad.request;break;default:bad.object.pool=nullptr;}
            expect(!origins.prepare_restart(identity,capture,bad));
        }
        CHECK(kernel.scheduler().complete_wait(ticket.value(),sched::WaitCompletion::ready));expect(origins.validate_bound(identity,capture,binding));
        activate(kernel,group);auto again=kernel.scheduler().register_current_wait(sched::ThreadWaitReason::child,any?0:group.child);CHECK(again);
        auto newer=binding;newer.ticket=again.value();expect(origins.rearm(identity,capture,binding,newer));expect(!origins.validate_bound(identity,capture,binding));binding=newer;
        const auto previous=p->resources().native_space;auto next=previous;++next.generation;next.root+=4096;
        auto update=origins.stage_space(&kernel.scheduler(),p->resources(),previous,next);CHECK(update);p->resources().native_space=next;expect(origins.commit_space(update.value(),&kernel.scheduler(),p->resources()));identity.space=next;
        auto permit=origins.prepare_restart(identity,capture,binding);expect(permit.ok());expect(permit.value().plan().frame.pc==site);
        expect(origins.consume_restart(permit.value(),identity,capture,binding));expect(!origins.consume_restart(permit.value(),identity,capture,binding));
        origins.retire(*thread);retire(kernel,group);
    }
}
} // namespace
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for(bool restart:{false,true})for(bool ready:{false,true})for(bool any:{false,true})publication(kernel,restart,ready,any);
    for(unsigned mode=0;mode<12;++mode)eligibility(kernel,mode);
    for(unsigned mode=0;mode<4;++mode)precedence(kernel,mode);
    for(bool signal:{false,true})peer_rearm(kernel,signal);
    nested(kernel);sender(kernel);ignored_and_late(kernel);for(unsigned mode=0;mode<5;++mode)teardown(kernel,mode);
    adapters<lu::Profile::i386>(kernel,7,{4,6,5,1,0,2});adapters<lu::Profile::x86_64>(kernel,14,{9,10,11,5,7,6});
    adapters<lu::Profile::aarch64>(kernel,8,{0,1,2,3,4,5});adapters<lu::Profile::arm32>(kernel,7,{0,1,2,3,4,5});
    adapters<lu::Profile::rv64>(kernel,17,{10,11,12,13,14,15});adapters<lu::Profile::rv32>(kernel,17,{10,11,12,13,14,15});
    adapters<lu::Profile::loongarch64>(kernel,11,{4,5,6,7,8,9});adapters<lu::Profile::mips>(kernel,2,{4,5,6,7,-1,-1});
    adapters<lu::Profile::mips64>(kernel,2,{4,5,6,7,8,9});adapters<lu::Profile::ppc>(kernel,0,{3,4,5,6,7,8});
    expect(native::entry_stacks.accounting().leased_slots==0);
    expect(native::child_wait_objects.accounting().allocations==native::child_wait_objects.accounting().destructions);
    std::printf("CHILD_WAIT_SIGNAL_PASS hosted=1 native=0 checks=%zu\n",checks);
}
