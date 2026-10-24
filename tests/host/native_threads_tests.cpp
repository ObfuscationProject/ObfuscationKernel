// Actual clone, selected-TCB exit and futex implementation with hosted copies.
#include "native_clock_model.hpp"
namespace
{
constexpr auto profile = lu::configured_native_profile();
using Bridge = lu::NativeBridge<profile>;
using Codec = lu::Codec<profile>;
namespace flags = user::linux_bootstrap::clone_flags;
constexpr bool big = Codec::profile.big_endian;
constexpr bool backwards = profile != lu::Profile::x86_64 && profile != lu::Profile::loongarch64;
constexpr uptr word = hw::user_begin + 128;
void put(usize offset, u32 value)
{
    for (usize i=0;i<4;++i) mock::memory[offset+i]=static_cast<std::byte>(value >> (8*(big ? 3-i : i)));
}
u32 get(usize offset)
{
    u32 result=0;
    for(usize i=0;i<4;++i) result|=static_cast<u32>(mock::memory[offset+i]) << (8*(big ? 3-i : i));
    return result;
}
void arguments(hw::UserFrame &frame, lu::Operation op, std::array<u64,5> values)
{
    const auto number=Codec::syscall_number(op);CHECK(number);
    if constexpr(profile==lu::Profile::x86_64)
    {
        frame.registers[14]=*number;
        constexpr usize slots[]{9,10,11,5,7};
        for(usize i=0;i<5;++i)frame.registers[slots[i]]=values[i];
    }
    else if constexpr(profile==lu::Profile::i386)
    {
        frame.registers[7]=*number;
        constexpr usize slots[]{4,6,5,1,0};
        for(usize i=0;i<5;++i)frame.registers[slots[i]]=values[i];
    }
    else
    {
        frame.registers[Codec::profile.number_register]=*number;
        for(usize i=0;i<5;++i)
        {
            if constexpr(profile==lu::Profile::mips)
                if(i==4) {put(frame.sp-hw::user_begin+16,values[i]);break;}
            frame.registers[Codec::profile.argument_registers[i]]=values[i];
        }
    }
}
i64 invoke(Kernel &kernel,sched::ProcessId pid,hw::UserFrame &frame,bool &complete)
{
    return user::linux_bootstrap::dispatch<profile>(kernel,pid,frame,complete,
        [&](hw::Syscall call,bool &done){return native::dispatch(kernel,pid,call,done);},
        [](sched::ThreadControlBlock &created){return native::prepare_native_thread(created);});
}
sched::ProcessId process(Kernel &kernel)
{
    auto p=kernel.scheduler().create_user_process("native-threads",kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}));CHECK(p);
    auto *owner=kernel.scheduler().find(p.value());owner->resources().personality=sched::UserPersonality::linux;
    auto &leader=owner->threads()[0];CHECK(native::prepare_native_thread(leader));
    leader.native_active=true;leader.native_frame.pc=hw::user_begin;
    Bridge::set_stack_pointer(leader.native_frame,hw::user_begin+4096);
    CHECK(kernel.scheduler().set_runnable(p.value()));
    CHECK(kernel.scheduler().activate_thread(p.value(),p.value()));return p.value();
}
sched::ThreadId clone(Kernel &kernel,sched::ProcessId pid,u32 extra,uptr ptid=hw::user_begin+132,uptr ctid=word)
{
    CHECK(kernel.scheduler().activate_thread(pid,pid));
    auto &frame=kernel.scheduler().current_thread()->native_frame;
    frame.fp_state[5]=0xa7;
    uptr tls=hw::user_begin+256;
    if constexpr(profile==lu::Profile::i386)
    {
        put(512,Bridge::i386_tls_entry);put(516,tls);put(520,0xfffff);put(524,0x51);tls=hw::user_begin+512;
    }
    const uptr stack=hw::user_begin+8192-(profile==lu::Profile::x86_64 ? 8 : 0);
    arguments(frame,lu::Operation::clone,{flags::shared_group|extra,stack,ptid,backwards ? tls : ctid,backwards ? ctid : tls});
    bool done=true;const auto result=invoke(kernel,pid,frame,done);CHECK(result>0 && done);
    const auto *child=kernel.scheduler().find_thread(pid,result);CHECK(child && child->native_active && !child->syscall_pending);
    CHECK(child->native_frame.sp==stack && child->native_frame.fp_state[5]==0xa7);
    auto returned=Bridge::decode_return(child->native_frame);CHECK(returned && returned.value().value==0 && returned.value().error==0);
    return result;
}
void exit(Kernel &kernel,sched::ProcessId pid,sched::ThreadId tid,bool group=false)
{
    CHECK(kernel.scheduler().activate_thread(pid,tid));
    bool done=true;CHECK(native::dispatch(kernel,pid,{group ? OK_SYS_EXIT_GROUP : OK_SYS_EXIT,{17}},done)==0 && !done);
}
void lifecycle(Kernel &kernel)
{
    const auto pid=process(kernel);auto *p=kernel.scheduler().find(pid);
    auto descriptors=kernel.posix().pipe(0);CHECK(descriptors);
    const auto descriptions=kernel.scheduler().open_description_count();
    p->resources().native_space={.root=0x9000,.slot=2,.generation=3};
    const auto child=clone(kernel,pid,flags::settls|flags::parent_settid|flags::child_settid|flags::child_cleartid);
    CHECK(get(132)==child && p->threads()[0].tls.clear_tid_address==0);
    put(128,0x99887766);
    // CHILD_SETTID belongs to first child execution, not parent clone return.
    CHECK(get(128)==0x99887766);
    p->threads()[0].native_active=false;
    CHECK(native::step(kernel));CHECK(get(128)==child && mock::last_frame.fp_state[5]==0xa7);
    put(128,static_cast<u32>(child));
    p->threads()[0].native_active=true;
    const auto shared=clone(kernel,pid,0),private_waiter=clone(kernel,pid,0);
    for(auto pair : {std::pair{shared,native::FutexKeyDomain::anonymous_shared_operation},
                    std::pair{private_waiter,native::FutexKeyDomain::private_address_space}})
    {
        CHECK(kernel.scheduler().activate_thread(pid,pair.first));kernel.scheduler().current_thread()->syscall_pending=true;
        native::FutexRequest request;request.address=word;request.value=child;request.big_endian=big;request.domain=pair.second;
        bool done=true;CHECK(native::futex(kernel,pid,request,done)==0 && !done);
    }
    exit(kernel,pid,child);
    CHECK(get(128)==0 && kernel.scheduler().find_thread(pid,child)->state==sched::ProcessState::exited);
    const auto *awoken=kernel.scheduler().find_thread(pid,shared);
    CHECK(awoken->state==sched::ProcessState::runnable && !awoken->syscall_pending);
    auto result=Bridge::decode_return(awoken->native_frame);CHECK(result && result.value().value==0 && !result.value().error);
    CHECK(kernel.scheduler().find_thread(pid,private_waiter)->wait_registration.active());
    CHECK(p->resources().native_space.root==0x9000 && p->fd_table().contains(descriptors.value()[0]));
    CHECK(kernel.scheduler().open_description_count()==descriptions);
    // Retired nonleader slots get fresh identities, even when repeatedly used.
    for(usize i=0;i<32;++i) {auto fresh=clone(kernel,pid,flags::parent_settid,1);CHECK(fresh>private_waiter);exit(kernel,pid,fresh);}
    exit(kernel,pid,pid);CHECK(p->state()!=sched::ProcessState::zombie && awoken->native_active);
    exit(kernel,pid,shared,true);CHECK(p->state()==sched::ProcessState::zombie && p->exit_code()==17);
    CHECK(!native::futex_objects.accounting().live_objects && !native::page_pool.object_count());
    CHECK(!p->resources().native_space.root && !p->fd_table().contains(descriptors.value()[0]));
    CHECK(kernel.scheduler().reap_orphans());
}
void key_permissions_and_last_exit(Kernel &kernel)
{
    const auto pid=process(kernel);auto *p=kernel.scheduler().find(pid);
    native::FutexRequest request;request.operation=native::FutexOperation::wake;request.address=word;request.value=1;
    bool done=true;mock::write_forbidden=word;
    CHECK(native::futex(kernel,pid,request,done)==0);
    request.domain=native::FutexKeyDomain::anonymous_shared_operation;
    CHECK(native::futex(kernel,pid,request,done)==-14);
    request.operation=native::FutexOperation::wait;
    CHECK(native::futex(kernel,pid,request,done)==-14 && done);
    mock::write_forbidden=0;
    put(128,123);p->threads()[0].tls.clear_tid_address=word;
    exit(kernel,pid,pid);CHECK(get(128)==123 && p->state()==sched::ProcessState::zombie);
    CHECK(kernel.scheduler().reap_orphans());
}
}
int main()
{
    static Kernel kernel;CHECK(kernel.boot({}));native::started=true;native::runtime_kernel=&kernel;
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    lifecycle(kernel);key_permissions_and_last_exit(kernel);
    const auto stacks=native::entry_stacks.accounting();
    CHECK(!stacks.leased_slots && !stacks.active_slots && !stacks.quarantined_slots);
    CHECK(stacks.reservations==stacks.releases && native::runtime_status());
    std::puts("PASS native thread clone TLS exit clear-TID domain tests guest=0 hardware_isolation=0 smp=0");
}
