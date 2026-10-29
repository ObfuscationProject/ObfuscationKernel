#include "ok/arch/mips/signal.hpp"
#include <cstdio>
#include <cstdlib>
using namespace ok;
namespace hw = arch::execution;
namespace cpu = hw::mips_o32_private;
namespace n = user::native;
using R = n::MipsO32SignalReadiness;
using Q = sched::PendingSignals<128>;
unsigned checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %u: %s\n", __LINE__, #x); std::abort(); } } while (0)
cpu::Facts valid_facts()
{ return {0x19300,0x80008482,0x9e190c8f,0x80000000,0,0x20000000,0x739300,0,true,true,true}; }
hw::UserFrame valid_frame()
{
    hw::UserFrame f{}; f.pc=0x40000000; f.sp=0x40008000;
    f.registers[29]=f.sp; f.flags=cpu::user_status; return f;
}
bool same(const hw::UserFrame &a,const hw::UserFrame &b)
{ return a.pc==b.pc && a.sp==b.sp && a.flags==b.flags && a.registers==b.registers &&
         a.fs_base==b.fs_base && a.gs_base==b.gs_base && a.fp_state==b.fp_state; }
int main()
{
    const auto facts=valid_facts(); CHECK(cpu::qualified(facts));
    constexpr u32 cpu::Facts::*fields[]={&cpu::Facts::prid,&cpu::Facts::config0,&cpu::Facts::config1,
        &cpu::Facts::config2,&cpu::Facts::config3,&cpu::Facts::status,&cpu::Facts::fir,&cpu::Facts::fcsr};
    for (auto field:fields) for(unsigned bit=0;bit<32;++bit) {
        auto bad=facts; bad.*field^=u32{1}<<bit; CHECK(!cpu::qualified(bad));
    }
    for(unsigned select=1;select<=3;++select) {
        auto bad=facts;
        if(select==1) bad.has_config1=false;
        if(select==2) bad.has_config2=false;
        if(select==3) bad.has_config3=false;
        CHECK(!cpu::qualified(bad));
    }
    auto frame=valid_frame(); CHECK(cpu::frame_mode(frame));
    for(unsigned bit=0;bit<sizeof(uptr)*8;++bit) {
        auto bad=frame; bad.flags^=uptr{1}<<bit; CHECK(!cpu::frame_mode(bad));
    }
    for(unsigned bit=0;bit<32;++bit) {
        auto f=frame; const u32 value=u32{1}<<bit;
        for(unsigned i=0;i<4;++i)f.fp_state[256+i]=static_cast<u8>(value>>(24-i*8));
        CHECK(cpu::fcsr(f)==value); CHECK(cpu::frame_mode(f)==!(value&0x007c0000));
    }
    for(unsigned rs=0;rs<32;++rs)for(unsigned rt=0;rt<32;++rt) {
        const bool a=rs!=26&&rs!=27,b=rt!=26&&rt!=27;
        for(u32 op:{4u,5u,20u,21u})CHECK(cpu::delay_operands_admitted((op<<26)|(rs<<21)|(rt<<16))==(a&&b));
        for(u32 op:{6u,7u,22u,23u})CHECK(cpu::delay_operands_admitted((op<<26)|(rs<<21)|(rt<<16))==a);
        CHECK(cpu::delay_operands_admitted((rs<<21)|(rt<<11)|9)==(a&&rs!=rt));
        const bool regimm=rt<4||(rt>=16&&rt<=19&&rs!=31);
        CHECK(cpu::delay_operands_admitted((1u<<26)|(rs<<21)|(rt<<16))==(a&&regimm));
    }
    cpu::CaptureTracker backend; CHECK(!backend.capability().admitted());
    CHECK(backend.qualify(facts));
    R::Identity id{0x1000,0,1,1,1},child_id{0x1000,1,2,2,1};
    R lease,child;
    CHECK(!lease.ready(id)); CHECK(!lease.publish_initial(id,frame,backend.capability()));
    lease.reset(id,backend.capability()); CHECK(lease.publish_initial(id,frame,backend.capability()));
    CHECK(lease.ready(id)); CHECK(!lease.publish_initial(id,frame,backend.capability()));
    CHECK(lease.context(id,backend.capability()).resume==n::MipsO32SignalContext::ResumeContract::normalized_resume_pc);
    child.reset(child_id,backend.capability());
    CHECK(!child.inherit_syscall(child_id,lease,id,frame,backend.capability()));
    CHECK(!lease.publish_sigreturn(id,frame,backend.capability()));
    for(unsigned cycle=0;cycle<1000;++cycle) {
        CHECK(lease.begin_entry(id,backend.capability()));
        backend.invalidate(); CHECK(!backend.query(frame).matches(frame));
        backend.publish(frame,cpu::CaptureKind::syscall_normalized);
        auto syscall=backend.query(frame); CHECK(syscall.matches(frame));
        CHECK(lease.capture(id,frame,syscall)); CHECK(lease.ready(id));
        child.reset(child_id,backend.capability());
        CHECK(child.inherit_syscall(child_id,lease,id,frame,backend.capability()));
        CHECK(child.ready(child_id)); CHECK(!child.ready(id));
        CHECK(lease.publish_sigreturn(id,frame,backend.capability()));
        CHECK(lease.ready(id)); CHECK(!lease.publish_sigreturn(id,frame,backend.capability()));
        CHECK(lease.begin_entry(id,backend.capability())); backend.invalidate();
        const auto saved=frame; backend.publish(frame,cpu::CaptureKind::interrupted_bd);
        CHECK(lease.capture(id,frame,backend.query(frame))); CHECK(!lease.ready(id));
        CHECK(lease.state()==R::State::interrupted_bd); CHECK(same(frame,saved));
        CHECK(!lease.publish_handler(id,frame,backend.capability()));
        child.reset(child_id,backend.capability()); CHECK(!child.inherit_syscall(child_id,lease,id,frame,backend.capability()));
        CHECK(lease.begin_entry(id,backend.capability())); backend.invalidate();
        CHECK(!lease.capture(id,frame,syscall)); CHECK(!lease.ready(id));
        CHECK(lease.begin_entry(id,backend.capability())); backend.invalidate();
        backend.publish(frame,cpu::CaptureKind::interrupted_non_bd);
        CHECK(lease.capture(id,frame,backend.query(frame))); CHECK(lease.ready(id));
        CHECK(lease.publish_handler(id,frame,backend.capability()));
        auto reused=id; ++reused.generation;
        lease.reset(reused,backend.capability()); CHECK(!lease.ready(id)); CHECK(!lease.ready(reused));
        CHECK(lease.begin_entry(reused,backend.capability()));
        CHECK(!lease.capture(reused,frame,backend.query(frame))); // pre-reuse capture cannot promote
        lease.reset(id,backend.capability()); CHECK(lease.publish_initial(id,frame,backend.capability()));
    }
    CHECK(lease.begin_entry(id,backend.capability())); backend.invalidate();
    backend.publish(frame,cpu::CaptureKind::interrupted_bd); CHECK(lease.capture(id,frame,backend.query(frame)));
    // An unverified backend observation (an unresolvable trap) does not promote readiness.
    {
        R other; R::Identity other_id{0x2000,0,3,3,1}; other.reset(other_id,backend.capability());
        CHECK(other.publish_initial(other_id,frame,backend.capability()));
        CHECK(other.begin_entry(other_id,backend.capability())); backend.invalidate();
        backend.publish(frame,cpu::CaptureKind::unverified); CHECK(!backend.query(frame).matches(frame));
        CHECK(!other.capture(other_id,frame,backend.query(frame))); CHECK(!other.ready(other_id));
        CHECK(other.state()==R::State::unverified);
    }
    Q queue; CHECK(queue.reset({1,1,1})); CHECK(queue.enqueue({16,-6,1,1})==Q::Enqueue::queued);
    sched::SignalActionTable actions; auto *action=actions.find(16); CHECK(action);
    action->handler=0x40001000; action->flags=0x88000008; action->mask.set(17);
    sched::SignalMask mask; sched::SignalAltstack stack{0x40002000,0x8000,0};
    Q::Delivery ticket,after; CHECK(queue.peek(mask,ticket));
    const auto prior_actions=actions; const auto prior_mask=mask; const auto prior_stack=stack; const auto prior_frame=frame;
    for(unsigned i=0;i<1000;++i) {
        CHECK(n::mips_o32_handler_gate(queue,actions,mask,lease,id,backend.capability())==n::MipsO32HandlerGate::defer_handler);
        CHECK(queue.peek(mask,after)&&after==ticket); CHECK(actions==prior_actions);
        CHECK(mask==prior_mask); CHECK(stack==prior_stack); CHECK(same(frame,prior_frame));
    }
    action->handler=sched::SignalAction::ignored_handler;
    CHECK(n::mips_o32_handler_gate(queue,actions,mask,lease,id,backend.capability())==n::MipsO32HandlerGate::proceed);
    action->handler=sched::SignalAction::default_handler;
    CHECK(n::mips_o32_handler_gate(queue,actions,mask,lease,id,backend.capability())==n::MipsO32HandlerGate::proceed);
    action->handler=0x40001000;
    // Without a capture the gate refuses; the runtime then terminates only this process.
    lease.revoke(id);
    CHECK(n::mips_o32_handler_gate(queue,actions,mask,lease,id,backend.capability())==n::MipsO32HandlerGate::unverified);
    CHECK(queue.peek(mask,after)&&after==ticket);
    // A resolved fault outside a delay slot (the backend publishes it as interrupted_non_bd) proceeds.
    CHECK(lease.begin_entry(id,backend.capability())); backend.invalidate();
    backend.publish(frame,cpu::CaptureKind::interrupted_non_bd); CHECK(lease.capture(id,frame,backend.query(frame)));
    CHECK(n::mips_o32_handler_gate(queue,actions,mask,lease,id,backend.capability())==n::MipsO32HandlerGate::proceed);
    action->flags=0;
    CHECK(lease.begin_entry(id,backend.capability())); backend.invalidate();
    backend.publish(frame,cpu::CaptureKind::syscall_normalized); CHECK(lease.capture(id,frame,backend.query(frame)));
    CHECK(n::mips_o32_handler_gate(queue,actions,mask,lease,id,backend.capability())==n::MipsO32HandlerGate::proceed);
    u32 selected{}; unsigned writes{};
    auto result=n::MipsO32SignalDelivery::deliver(queue,actions,mask,frame,stack,0x40000000,0x80000000,
        lease.context(id,backend.capability()),[&](u64,std::span<const std::byte>){++writes;return true;},selected,
        0x40001000,[](const hw::UserFrame&){return true;});
    CHECK(result==n::MipsO32SignalDelivery::Result::handler); CHECK(writes==1); CHECK(selected==16);
    CHECK(!queue.peek({},after)); CHECK(mask.contains(16));
    CHECK(lease.publish_handler(id,frame,backend.capability()));
    std::printf("MIPS_O32_ADMISSION_MODEL_PASS checks=%u\n",checks);
}
