#include "ok/user/native_signal_readiness_ppc32.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <vector>

using namespace ok;
namespace n=arch::execution;
namespace p=n::ppc32_private;
using R=user::native::Ppc32SignalReadiness;
using Image=user::native::Ppc32ImageAdmission;
using Personality=user::ppc32_elf_admission::Personality;
using Profile=user::ppc32_elf_admission::RuntimeProfile;
static usize checks{};
static void expect(bool v,const char *message)
{++checks;if(!v){std::fprintf(stderr,"FAIL %s at check %zu\n",message,checks);std::abort();}}
static n::AddressSpace live_space{0x12340000,2,3};
static bool space_live=true;
bool n::ppc32_private::address_space_live(n::AddressSpace s) noexcept {return space_live && p::same_space(s,live_space);}
namespace ok::arch::execution::ppc32_private::detail
{
struct ModelAccess
{
    static Tracker make() {return Tracker{};}
    static void qualify(Tracker &t,Facts f) {t.qualify_initialized(f);}
    static void enter(Tracker &t) {t.invalidate();}
    static void capture(Tracker &t,const UserFrame &f,AddressSpace s,const KernelEntryStackView &v,const Trap &trap)
    {t.publish_owned(f,s,v,trap);}
    static void exhaust(Tracker &t) {t.sequence_=~u64{};t.invalidate();}
};
}
using Access=p::detail::ModelAccess;
static_assert(!std::is_default_constructible_v<p::detail::Tracker>);
static_assert(!std::is_copy_constructible_v<p::detail::Tracker>);
static_assert(!std::is_copy_constructible_v<R>);
static_assert(!std::is_aggregate_v<p::Observation> && !std::is_aggregate_v<p::Capability> && !std::is_aggregate_v<Image>);
alignas(4096) static std::byte arena[memory::KernelEntryStackPool::arena_bytes];
alignas(4096) static std::byte other_arena[memory::KernelEntryStackPool::arena_bytes];
static n::UserFrame seed()
{
    n::UserFrame f{};for(usize i=0;i<32;++i)f.registers[i]=0x12340000+i;
    f.pc=0x40001000;f.sp=0x7ffffff0;f.fs_base=0x40008000;f.gs_base=0x40009000;f.flags=p::user_msr;
    f.registers[1]=f.sp;f.registers[2]=f.fs_base;f.registers[13]=f.gs_base;
    f.fp_state[267]=2;f.fp_state[280]=0xb4;f.fp_state[281]=0x68;f.fp_state[282]=0xac;f.fp_state[283]=0xe1;
    return f;
}
static std::vector<std::byte> read(const char *file)
{
    std::ifstream in(file,std::ios::binary);expect(bool(in),"open exact ELF");
    std::vector<char> raw((std::istreambuf_iterator<char>(in)),{});std::vector<std::byte> out(raw.size());
    for(usize i=0;i<raw.size();++i){out[i]=std::byte(static_cast<u8>(raw[i]));}
    return out;
}
int main(int argc,char **argv)
{
    expect(argc==3,"two exact ELF inputs");const auto linux_elf=read(argv[1]),native_elf=read(argv[2]);
    auto tracker=Access::make();const p::Facts facts{0x80210022,0,0,true};
    expect(!tracker.capability().admitted(),"uninitialized capability closed");
    expect(!Image::inspect(tracker.capability(),linux_elf,linux_elf.size(),Personality::linux_user,Profile::e500v2_scalar_soft_float).valid(tracker.capability()),"metadata cannot bypass CPU gate");
    for(unsigned field=0;field<3;++field)for(unsigned bit=0;bit<32;++bit)
    {
        auto bad=facts;if(field==0)bad.pvr^=u32{1}<<bit;else if(field==1)bad.msr^=u32{1}<<bit;else bad.spefscr^=u32{1}<<bit;
        Access::qualify(tracker,bad);expect(!tracker.capability().admitted(),"unsupported measured fact bit");
    }
    auto not_read=facts;not_read.spefscr_read=false;Access::qualify(tracker,not_read);expect(!tracker.capability().admitted(),"unmeasured SPEFSCR rejected");
    Access::qualify(tracker,facts);auto cap=tracker.capability();expect(cap.current(),"measured initialization accepted");
    auto image=Image::inspect(cap,linux_elf,linux_elf.size(),Personality::linux_user,Profile::e500v2_scalar_soft_float);
    auto native_image=Image::inspect(cap,native_elf,native_elf.size(),Personality::native_os,Profile::e500v2_scalar_soft_float);
    expect(image.valid(cap)&&native_image.valid(cap),"exact native and Linux metadata eligible");
    expect(!Image::inspect(cap,linux_elf,linux_elf.size(),Personality::native_os,Profile::e500v2_scalar_soft_float).valid(cap),"personality explicit");
    memory::KernelEntryStackPool pool,other;
    expect(pool.activate(arena).ok()&&other.activate(other_arena).ok(),"real pool metadata");
    const memory::KernelEntryStackOwner owner{11,12};auto h=pool.reserve(owner);auto other_h=other.reserve(owner);expect(h.ok()&&other_h.ok(),"real leases");
    auto active=pool.activate_entry(h.value(),owner,0);auto other_active=other.activate_entry(other_h.value(),owner,0);expect(active.ok()&&other_active.ok(),"two independently minted views");
    auto view=active.value();auto other_view=other_active.value();
    expect(!n::Ppc32PrivateViewAccess::same(view,other_view),"different real pool records with equal owner and counters stay distinct");
    auto frame=seed();R readiness;R::Identity id{live_space,owner.pid,owner.tid,1};readiness.reset(id,image);
    expect(!readiness.ready(id,frame,cap),"initial frame never inferred ready");
    auto capture=[&](u32 vector,n::TrapKind kind,u32 esr=0x00800000,u32 dear=0x60001234)
    {
        if(vector==6) frame.flags=p::user_msr|((esr>>8)&0x000e0000U);
        expect(readiness.begin_entry(id,frame,live_space,view,tracker.capability()),"begin capture");
        Access::enter(tracker);n::Trap trap{kind,vector,esr,dear,view.top()-p::entry_extent};
        Access::capture(tracker,frame,live_space,view,trap);
        auto observation=tracker.query(frame);
        expect(observation.valid_for(frame,live_space,view),"opaque event captured");
        expect(readiness.capture(id,frame,live_space,view,observation,tracker.capability()),"fresh exact capture consumed");
        return observation;
    };
    expect(!p::Capability{}.current() && !p::Observation{}.valid_for(frame,live_space,view),"default opaque tokens invalid");
    auto outsider=Access::make();Access::qualify(outsider,facts);
    expect(!image.valid(outsider.capability()),"foreign CPU issuer rejected");
    auto rejected_capture=[&](n::UserFrame candidate,n::Trap trap)
    {
        frame=candidate;Access::enter(tracker);Access::capture(tracker,frame,live_space,view,trap);
        expect(!tracker.query(frame).valid_for(frame,live_space,view),"invalid owned capture rejected");
    };
    const n::Trap syscall{n::TrapKind::syscall,8,0,0,view.top()-p::entry_extent};
    for(auto sp:{view.top()-p::entry_extent-16,view.top()-p::entry_extent+16,view.lower()-1,view.top(),ok::uptr{0}})
    {auto trap=syscall;trap.kernel_stack_pointer=sp;rejected_capture(seed(),trap);}
    for(auto bit:{0x00800000u,0x00400000u,0x00000080u})
    {auto invalid=seed();for(unsigned i=0;i<4;++i)invalid.fp_state[264+i]|=static_cast<u8>(bit>>(8*(3-i)));rejected_capture(invalid,syscall);}
    for(unsigned field=0;field<4;++field)
    {auto invalid=seed();if(field==0)invalid.flags^=1;if(field==1)invalid.registers[1]^=16;if(field==2)invalid.registers[2]^=16;if(field==3)invalid.registers[13]^=16;rejected_capture(invalid,syscall);}
    frame=seed();expect(readiness.begin_entry(id,frame,live_space,view,tracker.capability()),"begin then forged observation");
    expect(!readiness.capture(id,frame,live_space,view,{},tracker.capability()) && readiness.state()==R::State::unverified,"forged observation revokes awaiting state");
    expect(readiness.begin_entry(id,frame,live_space,view,tracker.capability()),"begin then failed entry");Access::enter(tracker);
    expect(!readiness.capture(id,frame,live_space,view,tracker.query(frame),tracker.capability()),"failed native entry cannot publish");
    auto original_space=live_space;auto wrong_space=live_space;wrong_space.generation++;
    expect(!readiness.begin_entry(id,frame,wrong_space,view,tracker.capability()),"wrong address-space entry rejected");
    auto wrong_owner=id;wrong_owner.pid++;readiness.reset(wrong_owner,image);
    expect(!readiness.begin_entry(wrong_owner,frame,live_space,view,tracker.capability()),"view owner must match identity");readiness.reset(id,image);
    R native_readiness;native_readiness.reset(id,native_image);
    expect(native_readiness.begin_entry(id,frame,live_space,view,tracker.capability()),"native personality can observe actual entry");
    Access::enter(tracker);Access::capture(tracker,frame,live_space,view,syscall);
    expect(native_readiness.capture(id,frame,live_space,view,tracker.query(frame),tracker.capability()),"native event consumed");
    expect(!native_readiness.complete_syscall(id,frame,tracker.capability(),{0,22}) && !native_readiness.context(id,frame,tracker.capability()).metadata_supplied,"native personality cannot mint Linux continuation");
    (void)original_space;
    frame=seed();
    auto observation=capture(8,n::TrapKind::syscall);
    p::Diagnostics d;
    expect(!readiness.ready(id,frame,tracker.capability()),"pending dispatch not signal-ready");
    expect(readiness.diagnostics(id,frame,tracker.capability(),d)&&d.original_r3==frame.registers[3]&&d.validity==p::original_r3_valid&&d.trap==0xc00,"original r3 before dispatch");
    const auto original=frame.registers[3];const auto cr=p::fp_word(frame,280);
    const auto before_completion=frame;const auto before_diagnostics=d;
    for(auto result:{abi::linux_user::Return{1,22},abi::linux_user::Return{0,4096}})
    {
        expect(!readiness.complete_syscall(id,frame,tracker.capability(),result),"invalid semantic result rejected");
        expect(p::same_frame(frame,before_completion) && readiness.diagnostics(id,frame,tracker.capability(),d) && d==before_diagnostics,"failed completion leaves frame and diagnostics intact");
    }
    expect(readiness.complete_syscall(id,frame,tracker.capability(),{0,22}),"ordinary negative result completion");
    expect(readiness.ready(id,frame,tracker.capability()),"completed actual syscall ready");
    expect(readiness.diagnostics(id,frame,tracker.capability(),d)&&d.original_r3==original&&d.result==0xffffffea&&d.validity==3,"signed negative result retained separately");
    expect(frame.registers[3]==22&&p::fp_word(frame,280)==(cr|0x10000000),"Linux user errno encoding");
    expect(!readiness.complete_syscall(id,frame,tracker.capability(),{1,0}),"completion one-shot");
    expect(!observation.valid_for(frame,live_space,view),"frame modification expires backend observation");
    auto context=readiness.context(id,frame,tracker.capability());expect(context.metadata_supplied&&context.metadata.result==0xffffffea,"trusted context output");
    context.metadata.trap=0xffffffff;context.metadata.original_r3=0;expect(readiness.context(id,frame,tracker.capability()).metadata.trap==0xc00,"returned diagnostics never grant authority");
    for(usize i=0;i<32;++i){frame.registers[i]^=1;expect(!readiness.ready(id,frame,tracker.capability()),"every scalar edit invalidates");frame.registers[i]^=1;}
    for(usize i=0;i<512;++i){frame.fp_state[i]^=1;expect(!readiness.ready(id,frame,tracker.capability()),"every extended-state edit invalidates");frame.fp_state[i]^=1;}
    for(auto ptr:{&frame.pc,&frame.sp,&frame.flags,&frame.fs_base,&frame.gs_base}){*ptr^=4;expect(!readiness.ready(id,frame,tracker.capability()),"every alias/control edit invalidates");*ptr^=4;}
    auto moved=frame;expect(!readiness.ready(id,moved,tracker.capability()),"same bytes at different frame rejected");
    for(unsigned field=0;field<6;++field){auto wrong=id;if(field==0)wrong.pid++;if(field==1)wrong.tid++;if(field==2)wrong.image_generation++;if(field==3)wrong.space.root++;if(field==4)wrong.space.slot++;if(field==5)wrong.space.generation++;expect(!readiness.ready(wrong,frame,tracker.capability()),"identity field bound");}
    expect(pool.deactivate_entry(h.value(),owner,0).ok(),"actual deactivate");
    expect(readiness.ready(id,frame,tracker.capability()),"consumed proof survives deactivation");
    expect(!observation.valid_for(frame,live_space,view),"observation requires live activation");
    active=pool.activate_entry(h.value(),owner,0);expect(active.ok(),"reactivate same lease");
    expect(!readiness.ready(id,frame,tracker.capability()),"new activation epoch revokes old proof");view=active.value();
    frame=seed();observation=capture(10,n::TrapKind::timer,0xffffffff,0xffffffff);
    expect(readiness.ready(id,frame,tracker.capability()),"timer fresh proof");
    expect(readiness.diagnostics(id,frame,tracker.capability(),d)&&d.trap==0x900&&!d.validity&&!d.original_r3&&!d.result&&!d.dar&&!d.dsisr&&d.raw_esr==0xffffffff,"timer zero irrelevant fields and keep raw syndrome");
    expect(!readiness.complete_syscall(id,frame,tracker.capability(),{0,0}),"timer cannot inherit syscall origin");
    expect(!observation.valid_for(frame,live_space,other_view),"different real pool view rejected");
    expect(readiness.begin_entry(id,frame,live_space,view,tracker.capability()),"await new entry");
    expect(!readiness.capture(id,frame,live_space,view,observation,tracker.capability()),"old sequence cannot recapture");
    for(auto vector:{2u,3u,6u,8u,10u,13u,14u,16u,17u,18u})
    {
        frame=seed();const auto kind=vector==8?n::TrapKind::syscall:vector==10?n::TrapKind::timer:(vector==2||vector==3||vector==13||vector==14)?n::TrapKind::page_fault:n::TrapKind::exception;
        capture(vector,kind,0xdeadbeef,0x60001234);expect(readiness.diagnostics(id,frame,tracker.capability(),d),"known event diagnostics");
        if(vector==3||vector==14)expect(d.trap==0x400&&d.dar==frame.pc&&!d.dsisr&&d.raw_esr==0xdeadbeef,"instruction ESR normalized with raw retained");
        if(vector==2||vector==13)expect(d.trap==0x300&&d.dar==0x60001234&&d.dsisr==0xdeadbeef,"data BookE syndrome");
        if(vector==6)expect(d.trap==0x700&&d.dsisr==0xdeadbeef&&!d.dar&&d.validity==p::dsisr_valid,"program syndrome validity");
        if(vector>=16)expect(d.trap==(vector==16?0x2010u:vector==17?0x2030u:0x2050u)&&!d.validity,"SPE trap slot mapping");
    }
    for(u32 vector=0;vector<256;++vector)for(u32 kind=0;kind<4;++kind)
    {
        n::Trap trap{static_cast<n::TrapKind>(kind),vector,0xaabbccdd,0xeeff0011,view.top()-p::entry_extent};
        const auto normalized=p::diagnose(frame,trap);
        const bool recognized=(vector==8&&kind==0)||(vector==10&&kind==1)||((vector==2||vector==3||vector==13||vector==14)&&kind==2)||((vector==6||vector==16||vector==17||vector==18)&&kind==3);
        expect((normalized.event!=p::Event::none)==recognized,"exhaustive vector/kind consistency");
    }
    frame=seed();capture(8,n::TrapKind::syscall);
    expect(readiness.complete_syscall(id,frame,tracker.capability(),{0x76543210,0}),"success result completion");
    expect(frame.registers[3]==0x76543210&&p::fp_word(frame,280)==(0xb468ace1&~0x10000000u),"ordinary success clears only SO");
    space_live=false;expect(!readiness.ready(id,frame,tracker.capability()),"destroyed address space rejected");space_live=true;
    readiness.revoke();expect(!readiness.ready(id,frame,tracker.capability()),"explicit mutation/exec revoke");
    R child;auto child_id=id;child_id.image_generation++;child.reset(child_id,image);expect(!child.ready(child_id,frame,tracker.capability()),"fork image token carries no event proof");
    frame=seed();capture(10,n::TrapKind::timer);auto previous_cap=tracker.capability();Access::enter(tracker);expect(!readiness.ready(id,frame,tracker.capability())&&!previous_cap.current(),"any reentry attempt revokes single-entry proof window");
    frame=seed();capture(10,n::TrapKind::timer);expect(pool.deactivate_entry(h.value(),owner,0).ok()&&pool.release(h.value(),owner).ok(),"release actual lease");expect(!readiness.ready(id,frame,tracker.capability()),"released generation never ready");
    auto reused=pool.reserve(owner);expect(reused.ok()&&reused.value().generation!=h.value().generation,"pool slot reused with fresh generation");expect(!readiness.ready(id,frame,tracker.capability()),"same owner new lease not inherited");
    expect(pool.release(reused.value(),owner).ok(),"release new lease");
    expect(other.deactivate_entry(other_h.value(),owner,0).ok()&&other.release(other_h.value(),owner).ok(),"release other pool");
    Access::qualify(tracker,facts);expect(!cap.admitted()&&!image.valid(tracker.capability()),"reinitialization epoch revokes old capability and image eligibility");
    Access::exhaust(tracker);expect(!tracker.capability().admitted(),"sequence overflow fails closed");
    std::printf("PPC_ADMISSION_MODEL_PASS checks=%zu scope=hosted-lifecycle\n",checks);
}
