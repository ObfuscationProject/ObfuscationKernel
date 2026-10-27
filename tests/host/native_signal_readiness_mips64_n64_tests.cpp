#define OK_MIPS64_N64_ADMISSION_MODEL 1
#include "ok/arch/mips64/signal.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include <cstdio>
#include <cstdlib>
#include <type_traits>
namespace cpu=ok::arch::execution::mips64_n64_private;
namespace hw=ok::arch::execution;
namespace sn=ok::user::native;
using namespace ok;
namespace ok::arch::execution::mips64_n64_private::detail {struct ModelAccess {Tracker tracker{};void exhaust_sequence(){tracker.sequence_=~u64{};}void exhaust_epoch(){tracker.epoch_=~u64{};}};}
static_assert(!std::is_default_constructible_v<cpu::detail::Tracker>);
static_assert(sizeof(hw::UserFrame)==816 && offsetof(hw::UserFrame,fp_state)==304 && sizeof(hw::Trap)==32);
static usize checks;
void check(bool value,int line) {++checks;if(!value){std::fprintf(stderr,"N64_LIFECYCLE_FAIL line=%d check=%zu\n",line,checks);std::exit(1);}}
#define CHECK(x) check(static_cast<bool>(x),__LINE__)
cpu::Facts facts(){return {0x10000,0x8000c482,0xfea3519b,0x80000000,0x80,0,0,0x24000080,0x007f0000,0,true,true,true,false,false};}
hw::UserFrame frame(){hw::UserFrame f;f.pc=0x40000100;f.sp=0x40002000;f.registers[29]=f.sp;f.flags=cpu::user_status;for(usize i=0;i<256;++i)f.fp_state[i]=static_cast<u8>(i);return f;}
using Ready=sn::Mips64N64SignalReadiness;
alignas(4096) std::array<std::byte,memory::KernelEntryStackPool::arena_bytes> arena;
int main()
{
    CHECK(!cpu::Capability{}.admitted());CHECK(!cpu::Observation{}.sequence());CHECK(cpu::qualified(facts()));
    auto good=facts();
    const auto words=std::array{&cpu::Facts::prid,&cpu::Facts::config0,&cpu::Facts::config1,&cpu::Facts::config2,&cpu::Facts::config3,&cpu::Facts::config4,&cpu::Facts::config5,&cpu::Facts::status,&cpu::Facts::fir,&cpu::Facts::fcsr};
    for(auto word:words)for(unsigned bit=0;bit<32;++bit){auto wrong=good;wrong.*word^=u32{1}<<bit;CHECK(!cpu::qualified(wrong));}
    for(auto flag:std::array{&cpu::Facts::has_config1,&cpu::Facts::has_config2,&cpu::Facts::has_config3,&cpu::Facts::has_config4,&cpu::Facts::has_config5}){auto wrong=good;wrong.*flag=!(wrong.*flag);CHECK(!cpu::qualified(wrong));}
    auto dsp=good;dsp.config3=0x80000c80;dsp.has_config4=true;CHECK(dsp.prid==good.prid && !cpu::platform(dsp));
    CHECK((good.fir&0xc0000)==0xc0000); // PS/3D do not add an unretained register bank.
    for(u32 rs=0;rs<32;++rs)for(u32 rt=0;rt<32;++rt)
    {
        const bool retained=rs!=26&&rs!=27&&rt!=26&&rt!=27;
        for(u32 op:std::array<u32,4>{4,5,20,21})CHECK(cpu::delay_operands_admitted((op<<26)|(rs<<21)|(rt<<16)|1)==retained);
        CHECK(cpu::delay_operands_admitted((rs<<21)|(rt<<11)|9)==(rs!=26&&rs!=27&&rs!=rt));
    }
    for(u32 rs=0;rs<32;++rs)
    {
        CHECK(cpu::delay_operands_admitted((rs<<21)|8)==(rs!=26&&rs!=27));
        for(u32 link:std::array<u32,4>{16,17,18,19})CHECK(cpu::delay_operands_admitted((1u<<26)|(rs<<21)|(link<<16)|1)==(rs!=26&&rs!=27&&rs!=31));
        for(u32 bit=6;bit<21;++bit)CHECK(!cpu::delay_operands_admitted((rs<<21)|8|(1u<<bit)));
    }
    CHECK(!cpu::delay_operands_admitted(0));CHECK(!cpu::delay_operands_admitted(0x0000000c));
    cpu::detail::ModelAccess issuer,other;auto& tracker=issuer.tracker;
    CHECK(tracker.qualify(good));CHECK(other.tracker.qualify(good));const auto first=tracker.capability();CHECK(first.admitted());
    memory::KernelEntryStackPool pool;CHECK(pool.activate(arena));auto reserved=pool.reserve({1,1});CHECK(reserved);const auto handle=reserved.value();auto top=pool.top(handle,{1,1});CHECK(top);
    hw::AddressSpace space{0x2000,2,7};
    Ready::Identity id{reinterpret_cast<uptr>(&pool),top.value()-memory::KernelEntryStackPool::usable_bytes,top.value(),handle.slot,1,1,handle.generation,1,space};
    CHECK(id.valid());Ready ready;auto f=frame();ready.reset(id,tracker.capability());CHECK(!ready.ready(id,f,tracker.capability()));
    auto zero=f;zero.flags=0;CHECK(!ready.publish_initial(id,zero,tracker.capability()));CHECK(ready.publish_initial(id,f,tracker.capability()));CHECK(!ready.publish_initial(id,f,tracker.capability()));CHECK(ready.ready(id,f,tracker.capability()));
    CHECK(!ready.ready(id,zero,tracker.capability()));CHECK(!ready.ready(id,f,other.tracker.capability()));
    auto changed=id;++changed.image_generation;CHECK(!ready.ready(changed,f,tracker.capability()));
    ready.revoke(id);CHECK(!ready.publish_initial(id,f,tracker.capability()));ready.reset(id,tracker.capability());CHECK(!ready.publish_initial(id,f,tracker.capability()));
    ++id.image_generation;ready.reset(id,tracker.capability());CHECK(ready.publish_initial(id,f,tracker.capability()));
    auto activate=[&](){auto active=pool.activate_entry(handle,{1,1},0);CHECK(active);return active.value();};
    auto deactivate=[&](){CHECK(pool.deactivate_entry(handle,{1,1},0));};
    auto capture=[&](hw::TrapKind kind,u64 cause=0){auto view=activate();CHECK(ready.begin_entry(id,f,space,view,tracker.capability()));tracker.invalidate();hw::Trap trap{kind,0,cause,0,view.top()-832};tracker.publish_owned(f,space,view,trap);auto observation=tracker.query(f);CHECK(observation.sequence()>0);CHECK(ready.capture(id,f,space,view,observation,tracker.capability()));deactivate();return observation;};
    auto syscall=capture(hw::TrapKind::syscall);CHECK(ready.ready(id,f,tracker.capability()));CHECK(ready.state()==Ready::State::syscall_normalized);
    auto old_frame=f;f.registers[2]=0xffffffffffffffe1ULL;f.registers[7]=1;CHECK(!ready.ready(id,f,tracker.capability()));CHECK(ready.publish_syscall_completion(id,f,tracker.capability()));CHECK(ready.ready(id,f,tracker.capability()));
    f.pc+=4;CHECK(!ready.publish_syscall_completion(id,f,tracker.capability()));f.pc-=4;
    Ready child;auto child_id=id;child_id.tid=2;child_id.slot=3;++child_id.lease_generation;auto child_frame=f;child_frame.registers[2]=0;child_frame.registers[7]=0;child_frame.fs_base=0xfedcba9876543210ULL;
    child.reset(child_id,tracker.capability());CHECK(child.inherit_syscall(child_id,ready,id,f,child_frame,tracker.capability()));CHECK(child.ready(child_id,child_frame,tracker.capability()));CHECK(!child.inherit_syscall(child_id,ready,id,f,child_frame,tracker.capability()));
    Ready grandchild;auto grandchild_id=child_id;++grandchild_id.tid;++grandchild_id.lease_generation;
    grandchild.reset(grandchild_id,tracker.capability());auto grandchild_frame=child_frame;
    CHECK(!grandchild.inherit_syscall(grandchild_id,child,child_id,child_frame,grandchild_frame,tracker.capability()));
    CHECK(!child.publish_sigreturn(child_id,child_frame,tracker.capability()));
    CHECK(!child.publish_syscall_completion(child_id,child_frame,tracker.capability()));
    auto view=activate();CHECK(ready.begin_entry(id,f,space,view,tracker.capability()));CHECK(!ready.capture(id,f,space,view,syscall,tracker.capability()));CHECK(!ready.ready(id,f,tracker.capability()));deactivate();
    view=activate();CHECK(ready.begin_entry(id,f,space,view,tracker.capability()));tracker.invalidate();tracker.publish_owned(f,space,view,{hw::TrapKind::syscall,8,0,0,view.top()-832});auto stale=tracker.query(f);deactivate();CHECK(!stale.valid_for(f,space,view));CHECK(!ready.capture(id,f,space,view,stale,tracker.capability()));
    view=activate();CHECK(ready.begin_entry(id,f,space,view,tracker.capability()));tracker.invalidate();tracker.publish_owned(f,space,view,{hw::TrapKind::syscall,8,0,0,view.top()-832});auto current=tracker.query(f);
    for(usize reg=0;reg<32;++reg){f.registers[reg]^=1;CHECK(!current.valid_for(f,space,view));f.registers[reg]^=1;}
    for(usize offset=0;offset<512;++offset){f.fp_state[offset]^=1;CHECK(!current.valid_for(f,space,view));f.fp_state[offset]^=1;}
    for(auto member:std::array{&hw::UserFrame::pc,&hw::UserFrame::sp,&hw::UserFrame::flags,&hw::UserFrame::fs_base,&hw::UserFrame::gs_base}){f.*member^=1;CHECK(!current.valid_for(f,space,view));f.*member^=1;}
    auto wrongspace=space;++wrongspace.generation;CHECK(!current.valid_for(f,wrongspace,view));CHECK(!current.from(other.tracker.capability()));CHECK(current.valid_for(f,space,view));
    CHECK(ready.begin_entry(id,f,space,view,tracker.capability()));CHECK(!ready.capture(id,f,space,view,current,tracker.capability()));
    tracker.invalidate();CHECK(!current.valid_for(f,space,view));CHECK(!ready.capture(id,f,space,view,current,tracker.capability()));deactivate();
    for(uptr bad:std::array<uptr,4>{0,id.lower-1,id.top,id.top-831}){view=activate();CHECK(ready.begin_entry(id,f,space,view,tracker.capability()));tracker.invalidate();tracker.publish_owned(f,space,view,{hw::TrapKind::syscall,8,0,0,bad});CHECK(!tracker.query(f).sequence());CHECK(!ready.capture(id,f,space,view,tracker.query(f),tracker.capability()));deactivate();}
    for(hw::TrapKind kind:std::array{hw::TrapKind::exception,hw::TrapKind::page_fault}){view=activate();CHECK(ready.begin_entry(id,f,space,view,tracker.capability()));tracker.invalidate();tracker.publish_owned(f,space,view,{kind,8,0,0,view.top()-832});CHECK(!tracker.query(f).sequence());deactivate();ready.revoke(id);}
    sched::PendingSignals<128> pending;CHECK(pending.reset({1,1,handle.generation}));CHECK(pending.enqueue({16,-6,41,7})==sched::PendingSignals<128>::Enqueue::queued);
    sched::SignalActionTable actions;actions.find(16)->handler=0x40000300;actions.find(16)->flags=0x88000008;
    sched::SignalAltstack altstack{0x40004000,8192,0};auto saved_altstack=altstack;auto saved_actions=actions;
    sched::PendingSignals<128>::Delivery ticket,again;CHECK(pending.peek({},ticket));
    capture(hw::TrapKind::timer,0xffffffff80008000ULL);CHECK(ready.state()==Ready::State::timer_bd);CHECK(ready.deferred(id,f,tracker.capability()));CHECK(!ready.ready(id,f,tracker.capability()));
    CHECK(sn::mips64_n64_handler_gate(pending,actions,{},ready,id,f,tracker.capability())==sn::Mips64N64HandlerGate::defer_handler);CHECK(pending.peek({},again)&&ticket==again);CHECK(actions==saved_actions&&altstack==saved_altstack);
    CHECK(ready.context(id,f,tracker.capability()).resume==sn::Mips64N64SignalFrame::ResumeContract::unverified);
    CHECK(!ready.publish_handler(id,f,tracker.capability()));CHECK(!ready.publish_sigreturn(id,f,tracker.capability()));
    f.registers[4]^=1;CHECK(sn::mips64_n64_handler_gate(pending,actions,{},ready,id,f,tracker.capability())==sn::Mips64N64HandlerGate::unverified);f.registers[4]^=1;
    actions.find(16)->handler=0;CHECK(sn::mips64_n64_handler_gate(pending,actions,{},ready,id,f,{})==sn::Mips64N64HandlerGate::proceed);
    actions.find(16)->handler=1;CHECK(sn::mips64_n64_handler_gate(pending,actions,{},ready,id,f,{})==sn::Mips64N64HandlerGate::proceed);actions=saved_actions;
    capture(hw::TrapKind::timer);CHECK(ready.state()==Ready::State::timer_non_bd&&ready.ready(id,f,tracker.capability()));
    Ready never_captured;auto fresh_id=child_id;++fresh_id.image_generation;never_captured.reset(fresh_id,tracker.capability());CHECK(!never_captured.inherit_syscall(fresh_id,ready,id,f,child_frame,tracker.capability()));
    capture(hw::TrapKind::syscall);CHECK(sn::mips64_n64_handler_gate(pending,actions,{},ready,id,f,tracker.capability())==sn::Mips64N64HandlerGate::proceed);CHECK(pending.peek({},again)&&ticket==again);
    f.pc=0x40000300;f.sp=0x40006000;f.registers[29]=f.sp;CHECK(ready.publish_handler(id,f,tracker.capability()));CHECK(ready.state()==Ready::State::handler);
    CHECK(!ready.publish_sigreturn(id,f,tracker.capability()));capture(hw::TrapKind::syscall);f=old_frame;CHECK(ready.publish_sigreturn(id,f,tracker.capability()));CHECK(ready.state()==Ready::State::sigreturn);CHECK(!ready.publish_syscall_completion(id,f,tracker.capability()));
    view=activate();
    for(auto member:std::array{&Ready::Identity::pid,&Ready::Identity::tid,&Ready::Identity::lease_generation,&Ready::Identity::image_generation}) {
        auto wrong_id=id;wrong_id.*member+=1;auto rejected=ready;
        CHECK(!rejected.begin_entry(wrong_id,f,space,view,tracker.capability()));CHECK(rejected.state()==Ready::State::unverified);
    }
    {auto wrong_id=id;++wrong_id.slot;auto rejected=ready;CHECK(!rejected.begin_entry(wrong_id,f,space,view,tracker.capability()));}
    {auto wrong_id=id;++wrong_id.pool;auto rejected=ready;CHECK(!rejected.begin_entry(wrong_id,f,space,view,tracker.capability()));}
    {auto rejected=ready;CHECK(!rejected.begin_entry(id,f,space,view,{}));CHECK(rejected.state()==Ready::State::unverified);}
    {auto wrong_space=space;++wrong_space.generation;auto rejected=ready;CHECK(!rejected.begin_entry(id,f,wrong_space,view,tracker.capability()));}
    deactivate();
    const auto prior_capability=tracker.capability();CHECK(tracker.qualify(good));CHECK(!prior_capability.admitted());CHECK(!ready.ready(id,f,tracker.capability()));
    ready.revoke(id);CHECK(pool.release(handle,{1,1}));auto reused=pool.reserve({1,1});CHECK(reused&&reused.value().generation!=handle.generation);CHECK(!pool.activate_entry(handle,{1,1},0));
    Ready old_lease;old_lease.reset(id,tracker.capability());f=frame();CHECK(old_lease.publish_initial(id,f,tracker.capability()));
    auto new_view=pool.activate_entry(reused.value(),{1,1},0);CHECK(new_view);
    CHECK(new_view.value().lower()==id.lower && new_view.value().top()==id.top);
    CHECK(!old_lease.begin_entry(id,f,space,new_view.value(),tracker.capability()));
    CHECK(pool.deactivate_entry(reused.value(),{1,1},0));CHECK(pool.release(reused.value(),{1,1}));
    // Even colliding trusted labels/bounds from two separately minted pool
    // records must not substitute the view captured by begin_entry.
    memory::KernelEntryStackPool alias_a,alias_b;
    CHECK(alias_a.activate(arena));CHECK(alias_b.activate(arena));
    auto lease_a=alias_a.reserve({3,4}),lease_b=alias_b.reserve({3,4});CHECK(lease_a&&lease_b);
    auto a_view=alias_a.activate_entry(lease_a.value(),{3,4},0);
    auto b_view=alias_b.activate_entry(lease_b.value(),{3,4},0);CHECK(a_view&&b_view);
    CHECK(a_view.value().lower()==b_view.value().lower() && lease_a.value().generation==lease_b.value().generation);
    Ready::Identity alias_id{reinterpret_cast<uptr>(&alias_a),a_view.value().lower(),a_view.value().top(),lease_a.value().slot,3,4,lease_a.value().generation,1,space};
    Ready alias_ready;alias_ready.reset(alias_id,tracker.capability());
    CHECK(alias_ready.begin_entry(alias_id,f,space,a_view.value(),tracker.capability()));
    tracker.invalidate();tracker.publish_owned(f,space,b_view.value(),{hw::TrapKind::syscall,8,0,0,b_view.value().top()-832});
    CHECK(tracker.query(f).valid_for(f,space,b_view.value()));
    CHECK(!alias_ready.capture(alias_id,f,space,b_view.value(),tracker.query(f),tracker.capability()));
    CHECK(alias_a.deactivate_entry(lease_a.value(),{3,4},0));CHECK(alias_b.deactivate_entry(lease_b.value(),{3,4},0));
    cpu::detail::ModelAccess exhausted;CHECK(exhausted.tracker.qualify(good));
    exhausted.exhaust_sequence();auto exhausted_cap=exhausted.tracker.capability();exhausted.tracker.invalidate();CHECK(!exhausted_cap.admitted());
    exhausted.exhaust_epoch();CHECK(!exhausted.tracker.qualify(good));
    std::printf("N64_LIFECYCLE_PASS checks=%zu scope=host-model not-hardware=1\n",checks);
}
