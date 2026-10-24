// Actual signal-mask adapter and native inheritance with hosted mappings only.
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
    CHECK(offset <= mock::memory.size() - 4);
    for (usize i=0;i<4;++i) mock::memory[offset+i]=static_cast<std::byte>(value >> (8*(big ? 3-i : i)));
}
u32 get(usize offset)
{
    u32 result=0;
    for(usize i=0;i<4;++i) result|=static_cast<u32>(mock::memory[offset+i]) << (8*(big ? 3-i : i));
    return result;
}
void arguments(hw::UserFrame &frame, lu::Operation op, std::array<u64,5> values, usize count = 5)
{
    const auto number=Codec::syscall_number(op);CHECK(number);
    if constexpr(profile==lu::Profile::x86_64)
    {
        frame.registers[14]=*number;
        constexpr usize slots[]{9,10,11,5,7};
        for(usize i=0;i<count;++i)frame.registers[slots[i]]=values[i];
    }
    else if constexpr(profile==lu::Profile::i386)
    {
        frame.registers[7]=*number;
        constexpr usize slots[]{4,6,5,1,0};
        for(usize i=0;i<count;++i)frame.registers[slots[i]]=values[i];
    }
    else
    {
        frame.registers[Codec::profile.number_register]=*number;
        for(usize i=0;i<count;++i)
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
    auto p=kernel.scheduler().create_user_process("native-robust",kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}));CHECK(p);
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
using Mask = sched::SignalMask;
using Masks = native::SignalMaskCodec<profile>;
i64 mask_call(Kernel &kernel, sched::ProcessId pid, i32 how, uptr input, uptr output, usize size = Masks::bytes)
{
    auto &frame = kernel.scheduler().current_thread()->native_frame;
    arguments(frame, lu::Operation::rt_sigprocmask, {static_cast<u32>(how),input,output,size,0}, 4);
    bool complete = true; const auto result = invoke(kernel,pid,frame,complete); CHECK(complete); return result;
}
void bytes(Mask mask)
{
    const auto encoded = Masks::encode(mask);
    std::memcpy(mock::memory.data()+128,encoded.data(),encoded.size());
}
void syscall_masks(Kernel &kernel)
{
    const auto pid = process(kernel); auto *p = kernel.scheduler().find(pid);
    auto &mask = p->threads()[0].native_signal_mask;
    CHECK(mask == Mask{});
    CHECK(mask_call(kernel,pid,999,0,word)==0 && get(128)==0);
    CHECK(mask_call(kernel,pid,999,0,0,Masks::bytes+1)==-22);
    Mask requested; requested.set(10); requested.set(9); requested.set(Masks::stop);
    if constexpr(Masks::mips) requested.set(128);
    bytes(requested); CHECK(mask_call(kernel,pid,Masks::replace,word,word)==0);
    CHECK(mask.contains(10) && !mask.contains(9) && !mask.contains(Masks::stop));
    CHECK(mask.contains(128)==Masks::mips && get(128)==0);
    Mask second; second.set(12); bytes(second); mock::write_forbidden=word;
    CHECK(mask_call(kernel,pid,Masks::replace,word,word)==-14 && mask==second);
    mock::write_forbidden=0; mock::forbidden=word;
    CHECK(mask_call(kernel,pid,999,word,0)==-14 && mask==second);
    mock::forbidden=0; CHECK(mask_call(kernel,pid,999,word,0)==-22 && mask==second);
    exit(kernel,pid,pid); CHECK(kernel.scheduler().reap_orphans());
}
void inherited_masks(Kernel &kernel)
{
    const auto pid=process(kernel); auto *p=kernel.scheduler().find(pid);
    Mask leader_mask; leader_mask.set(10); p->threads()[0].native_signal_mask=leader_mask;
    const auto peer=clone(kernel,pid,0); auto *caller=kernel.scheduler().find_thread(pid,peer);
    CHECK(caller->native_signal_mask==leader_mask);
    CHECK(kernel.scheduler().activate_thread(pid,peer));
    Mask peer_mask; peer_mask.set(12); if constexpr(Masks::mips) peer_mask.set(65);
    bytes(peer_mask); CHECK(mask_call(kernel,pid,Masks::replace,word,0)==0);
    CHECK(caller->native_signal_mask==peer_mask && p->threads()[0].native_signal_mask==leader_mask);
    // The raw MIPS fifth clone argument must be in writable caller stack space.
    Bridge::set_stack_pointer(caller->native_frame,hw::user_begin+4096);
    arguments(caller->native_frame,lu::Operation::clone,{flags::shared_group,hw::user_begin+6144,0,0,0});
    bool complete=true; auto child=invoke(kernel,pid,caller->native_frame,complete);
    CHECK(child>0 && complete && kernel.scheduler().find_thread(pid,child)->native_signal_mask==peer_mask);
    auto forked=kernel.scheduler().fork_process(pid,false); CHECK(forked);
    auto *fork=kernel.scheduler().find(forked.value());
    CHECK(fork->threads()[0].native_signal_mask==peer_mask);
    fork->threads()[0].native_signal_mask.set(12,false);
    CHECK(caller->native_signal_mask==peer_mask);
    native::finish(kernel,forked.value(),0); CHECK(kernel.posix().wait(pid,forked.value()));
    // Exercise the mask-preserving install mechanism; actual exec argument/load
    // and old-MM ownership use a separate executed-dispatch regression.
    const auto saved=caller->native_signal_mask;
    auto replaced=native::replace_entry_stacks(native::entry_stacks,*p,[&] {
        return kernel.scheduler().replace_image(pid,kernel.arch().make_user_context({hw::user_begin,hw::user_begin+4096}),{});
    }); CHECK(replaced && replaced.committed);
    native::Image image; image.personality=sched::UserPersonality::linux;
    image.frame=hw::initial_frame(hw::user_begin,hw::user_begin+4096);
    native::install(*p,image,saved); CHECK(p->threads()[0].native_signal_mask==peer_mask);
    native::install(*p,image); CHECK(p->threads()[0].native_signal_mask==Mask{});
    exit(kernel,pid,pid); CHECK(kernel.scheduler().reap_orphans());
}
}
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); native::started=true; native::runtime_kernel=&kernel;
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    syscall_masks(kernel); inherited_masks(kernel);
    const auto stacks=native::entry_stacks.accounting();
    CHECK(!stacks.leased_slots && !stacks.active_slots && !stacks.quarantined_slots);
    CHECK(stacks.reservations==stacks.releases && native::runtime_status());
    std::puts("PASS native signal mask syscall inheritance tests guest=0 hardware_isolation=0 smp=0");
}
