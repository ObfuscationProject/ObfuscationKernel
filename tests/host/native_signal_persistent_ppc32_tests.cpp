#include "ok/arch/ppc/signal.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <vector>
using namespace ok;
namespace n=arch::execution;
namespace p=n::ppc32_private;
using R=user::native::Ppc32PersistentReadiness;
using Lease=user::native::Ppc32OwnedLease;
using Image=user::native::Ppc32ImageAdmission;
static usize checks{};
static void check(bool value,const char *label){++checks;if(!value){std::fprintf(stderr,"FAIL %s check=%zu\n",label,checks);std::abort();}}
namespace ok::arch::execution::ppc32_private::detail {
struct ModelAccess {
 static Tracker make(){return Tracker{};}
 static void qualify(Tracker &t){t.qualify_initialized({0x80210022,0,0,true});}
 static void enter(Tracker &t){t.invalidate();}
 static void capture(Tracker &t,const UserFrame &f,AddressSpace s,const KernelEntryStackView &v,const Trap &raw){t.publish_owned(f,s,v,raw);}
}; }
using Access=p::detail::ModelAccess;
static std::array<n::AddressSpace,6> spaces{{{0x1000,1,1},{0x2000,2,1},{0x3000,3,1},{0x4000,4,1},{0x5000,5,1},{0x6000,6,1}}};
static std::array<bool,6> live{{true,true,true,true,true,true}};
bool n::ppc32_private::address_space_live(n::AddressSpace s) noexcept {for(usize i=0;i<spaces.size();++i)if(live[i]&&p::same_space(s,spaces[i]))return true;return false;}
alignas(4096) static std::byte arena[memory::KernelEntryStackPool::arena_bytes],other_arena[memory::KernelEntryStackPool::arena_bytes];
static n::UserFrame seed(u32 tag=0x1234)
{
 n::UserFrame f{};for(usize i=0;i<32;++i)f.registers[i]=0x40000000+(tag<<4)+i;
 for(usize i=0;i<f.fp_state.size();++i)f.fp_state[i]=static_cast<u8>(i*37+tag);
 f.registers[0]=4;f.pc=0x40001000;f.sp=0x7ffff000;f.fs_base=0x40009000;f.gs_base=0x4000a000;f.flags=p::user_msr;
 f.registers[1]=f.sp;f.registers[2]=f.fs_base;f.registers[13]=f.gs_base;
 for(usize i=264;i<268;++i)f.fp_state[i]=i==267?2:0;
 return f;
}
static std::vector<std::byte> read(const char *path)
{
 std::ifstream in(path,std::ios::binary);check(bool(in),"exact Linux input");std::vector<char> raw((std::istreambuf_iterator<char>(in)),{});std::vector<std::byte> out(raw.size());
 for(usize i=0;i<raw.size();++i){out[i]=std::byte(static_cast<u8>(raw[i]));}
    return out;
}
static_assert(!std::is_copy_constructible_v<R> && !std::is_move_constructible_v<R> && !std::is_aggregate_v<Lease>);
int main(int argc,char **argv)
{
 check(argc==2,"one explicit ELF input");auto elf=read(argv[1]);auto tracker=Access::make();Access::qualify(tracker);
 auto cap=[&]{return tracker.capability();};
 auto image=Image::inspect(cap(),elf,elf.size(),user::ppc32_elf_admission::Personality::linux_user,user::ppc32_elf_admission::RuntimeProfile::e500v2_scalar_soft_float);
 memory::KernelEntryStackPool pool,other;check(pool.activate(arena).ok()&&other.activate(other_arena).ok(),"real pool metadata");
 const memory::KernelEntryStackOwner ao{11,12},bo{21,22},co{31,32},go{41,42};
 auto ah=pool.reserve(ao),bh=pool.reserve(bo),ch=pool.reserve(co),gh=pool.reserve(go),oh=other.reserve(ao);check(ah&&bh&&ch&&gh&&oh,"five actual reserved handles");
 auto al=Lease::inspect(pool,ah.value(),ao),bl=Lease::inspect(pool,bh.value(),bo),cl=Lease::inspect(pool,ch.value(),co),gl=Lease::inspect(pool,gh.value(),go);
 check(al.inactive()&&bl.inactive()&&cl.inactive()&&!Lease{}.inactive(),"opaque inactive lease inspection");
 check(!Lease::inspect(other,ah.value(),ao).inactive()&&!Lease::inspect(pool,ah.value(),bo).inactive(),"foreign pool and owner rejected");
 auto av=pool.activate_entry(ah.value(),ao,0),ov=other.activate_entry(oh.value(),ao,0);check(av&&ov,"simultaneous separate pool records");
 check(al.active(av.value())&&!al.active(ov.value()),"exact real record with equal owner generation epoch");
 check(!al.inactive()&&!Lease::inspect(pool,ah.value(),ao).inactive(),"active record cannot be inspected as inactive");
 check(pool.deactivate_entry(ah.value(),ao,0).ok()&&other.deactivate_entry(oh.value(),ao,0).ok()&&other.release(oh.value(),ao).ok(),"end record control");
 R a,b,c,g;R::Identity ai{spaces[0],ao.pid,ao.tid,1},bi{spaces[1],bo.pid,bo.tid,1},ci{spaces[2],co.pid,co.tid,1},gi{spaces[3],go.pid,go.tid,1};
 check(a.reset(ai,image,al,cap())&&b.reset(bi,image,bl,cap())&&c.reset(ci,image,cl,cap())&&g.reset(gi,image,gl,cap()),"bind canonical helpers");
 auto fa=seed(),fb=seed(0x2345),fc=seed(),fg=seed();check(!a.ready(ai,fa,cap())&&!a.pending(ai,fa,cap()),"image and initial shape never ready");
 auto capture=[&](R &r,R::Identity id,n::UserFrame &f,memory::KernelEntryStackHandle h,u32 vector,n::TrapKind kind,u32 esr=0,u32 dear=0)
 {
  memory::KernelEntryStackOwner owner{id.pid,id.tid};auto view=pool.activate_entry(h,owner,0);check(view.ok(),"activate exact lease");
  check(!r.ready(id,f,cap())&&!r.pending(id,f,cap()),"own reactivation expires parked proof");
  check(r.begin_entry(id,f,id.space,view.value(),cap()),"begin actual owned capture");Access::enter(tracker);
  if(vector==6)f.flags=p::user_msr|((esr>>8)&0x000e0000U);
  n::Trap raw{kind,vector,esr,dear,view.value().top()-p::entry_extent};Access::capture(tracker,f,id.space,view.value(),raw);
  auto observation=tracker.query(f);check(r.capture(id,f,id.space,view.value(),observation,cap()),"consume genuine model backend observation");
  check(!r.ready(id,f,cap())&&!r.pending(id,f,cap()),"no persistent use while owned entry active");
  check(pool.deactivate_entry(h,owner,0).ok(),"deactivate consumed lease");
  check(!observation.valid_for(f,id.space,view.value()),"raw observation cannot outlive activation");
 };
 for(unsigned scenario=0;scenario<4;++scenario)
 {
  auto view=pool.activate_entry(ah.value(),ao,0);check(view.ok(),"negative capture active lease");
  if(scenario!=0)check(a.begin_entry(ai,fa,ai.space,view.value(),cap()),"negative capture staged entry");
  Access::enter(tracker);n::Trap raw{n::TrapKind::syscall,8,0,0,view.value().top()-p::entry_extent};Access::capture(tracker,fa,ai.space,view.value(),raw);
  auto observation=tracker.query(fa);check(observation.valid_for(fa,ai.space,view.value()),"negative source is an actual opaque observation");
  auto moved=fa;if(scenario==3)Access::enter(tracker);
  check(!a.capture(ai,scenario==2?moved:fa,ai.space,view.value(),scenario==1?p::Observation{}:observation,cap()),"unstaged default copied or stale observation rejected");
  check(pool.deactivate_entry(ah.value(),ao,0).ok()&&!a.ready(ai,fa,cap())&&!a.pending(ai,fa,cap()),"failed capture closes publication");
 }
 capture(a,ai,fa,ah.value(),8,n::TrapKind::syscall,0x00800000,0x60001234);
 p::Diagnostics da,db;check(a.pending(ai,fa,cap())&&!a.ready(ai,fa,cap())&&a.diagnostics(ai,fa,cap(),da),"actual syscall pending");
 const auto before=fa;const auto raw=da;auto cached=cap();
 capture(b,bi,fb,bh.value(),10,n::TrapKind::timer,0xffffffff,0xffffffff);
 check(a.pending(ai,fa,cap())&&a.pending(ai,fa,cached)&&b.ready(bi,fb,cap()),"other owner preserves parked pending proof");
 Access::enter(tracker);check(a.pending(ai,fa,cap())&&b.ready(bi,fb,cap()),"unrelated failed entry does not expire consumed records");
 for(auto result:{R::Return{1,22},R::Return{0,4096},R::Return{0x100000000ULL,0}})
 {check(!a.complete_syscall(ai,fa,cap(),result),"invalid completion rejected");check(p::same_frame(fa,before)&&a.diagnostics(ai,fa,cap(),da)&&da==raw,"invalid completion atomic");}
 check(a.complete_syscall(ai,fa,cap(),{0,22}),"blocked completion after other owner entry");
 auto expected=before;check(R::Bridge::encode_return(expected,{0,22}).ok()&&p::same_frame(fa,expected),"exact r3 and CR0.SO ordinary effects");
 check(a.diagnostics(ai,fa,cap(),da)&&da.original_r3==raw.original_r3&&da.result==0xffffffea&&da.raw_esr==raw.raw_esr&&da.raw_dear==raw.raw_dear&&da.validity==3,"raw diagnostics preserved across completion");
 check(!a.complete_syscall(ai,fa,cap(),{0,0}),"ordinary completion one shot");
 check(b.diagnostics(bi,fb,cap(),db)&&db.event==p::Event::timer&&!db.validity&&!db.original_r3&&!db.result,"timer origin never inherits syscall");
 check(!b.complete_syscall(bi,fb,cap(),{0,0}),"timer refuses completion");
 for(usize i=0;i<32;++i){fa.registers[i]^=1;check(!a.ready(ai,fa,cap()),"all scalar snapshot fields bound");fa.registers[i]^=1;}
 for(usize i=0;i<512;++i){fa.fp_state[i]^=1;check(!a.ready(ai,fa,cap()),"all SPE and extended snapshot bytes bound");fa.fp_state[i]^=1;}
 for(auto ptr:{&fa.pc,&fa.sp,&fa.flags,&fa.fs_base,&fa.gs_base}){*ptr^=4;check(!a.ready(ai,fa,cap()),"control and alias fields bound");*ptr^=4;}
 auto copy=fa;check(!a.ready(ai,copy,cap()),"identical bytes at another address not canonical");
 for(unsigned field=0;field<6;++field){auto wrong=ai;if(field==0)wrong.pid++;if(field==1)wrong.tid++;if(field==2)wrong.image_generation++;if(field==3)wrong.space.root++;if(field==4)wrong.space.slot++;if(field==5)wrong.space.generation++;check(!a.ready(wrong,fa,cap()),"all owner space image fields bound");}
 auto foreign=Access::make();Access::qualify(foreign);check(!a.ready(ai,fa,foreign.capability()),"foreign CPU issuer rejected");
 auto renewed=pool.activate_entry(ah.value(),ao,0);check(renewed.ok()&&pool.deactivate_entry(ah.value(),ao,0).ok(),"same lease activation without publication");
 check(!a.ready(ai,fa,cap()),"same record epoch change persists after deactivation");
 fa=seed();capture(a,ai,fa,ah.value(),8,n::TrapKind::syscall);
 expected=fa;check(R::Bridge::encode_return(expected,{0x12345678,0}).ok(),"stage adapter success");
 fa=expected;fa.registers[4]^=1;check(!a.publish_linux_completion(ai,fa,cap(),{0x12345678,0}),"adapter cannot smuggle extra scalar edit");fa.registers[4]^=1;
 check(!a.publish_linux_completion(ai,fa,cap(),{0x12345679,0}),"encoded result must match semantic result");
 check(a.publish_linux_completion(ai,fa,cap(),{0x12345678,0})&&a.ready(ai,fa,cap()),"exact post-adapter completion accepted");
 fa=seed();capture(a,ai,fa,ah.value(),8,n::TrapKind::syscall);check(a.stage_relocation(ai,fa,cap()),"stage blocked frame relocation");
 auto moved=fa;fa={};check(!a.pending(ai,moved,cap()),"no query during pending relocation");
 check(a.commit_relocation(ai,moved,cap())&&a.pending(ai,moved,cap())&&!a.pending(ai,fa,cap()),"commit after source erased preserves blocked origin");
 check(a.complete_syscall(ai,moved,cap(),{77,0}),"relocated actual syscall can complete");
 check(a.stage_relocation(ai,moved,cap()),"stage relocated ready frame");auto bad=moved;bad.fp_state[255]^=1;
 check(!a.commit_relocation(ai,bad,cap())&&!a.ready(ai,moved,cap()),"bad relocation revokes stale source");
 fa=seed();capture(a,ai,fa,ah.value(),10,n::TrapKind::timer);check(a.stage_relocation(ai,fa,cap()),"stage canceled relocation");a.cancel();check(!a.ready(ai,fa,cap())&&!a.commit_relocation(ai,copy,cap()),"canceled relocation cannot republish");
 capture(a,ai,fa,ah.value(),10,n::TrapKind::timer);check(a.stage_space(ai,fa,spaces[4],cap()),"stage committed space replacement");
 auto next=ai;next.space=spaces[4];live[0]=false;check(a.commit_space(ai,next,fa,cap()),"old root may retire between stage and commit");ai=next;
 check(a.ready(ai,fa,cap())&&!a.ready({spaces[0],ao.pid,ao.tid,1},fa,cap()),"space identity changed without losing timer proof");
 check(a.diagnostics(ai,fa,cap(),da)&&da.event==p::Event::timer&&!da.validity,"space publication retains diagnostic origin");
 check(a.stage_space(ai,fa,spaces[5],cap()),"stage unpublished space");live[5]=false;next=ai;next.space=spaces[5];check(!a.commit_space(ai,next,fa,cap())&&!a.ready(ai,fa,cap()),"uncommitted target space fails closed");live[5]=true;
 capture(a,ai,fa,ah.value(),10,n::TrapKind::timer);bi.space=ai.space;check(b.reset(bi,image,bl,cap()),"bind shared VM peer");capture(b,bi,fb,bh.value(),10,n::TrapKind::timer);
 check(a.stage_space(ai,fa,spaces[5],cap())&&b.stage_space(bi,fb,spaces[5],cap()),"stage every shared VM owner");
 auto na=ai,nb=bi;na.space=nb.space=spaces[5];live[4]=false;check(a.commit_space(ai,na,fa,cap())&&!b.ready(bi,fb,cap()),"publishing one shared owner does not infer peer publication");
 check(b.commit_space(bi,nb,fb,cap()),"commit explicit shared peer");ai=na;bi=nb;
 auto handler=fa;handler.pc+=0x100;handler.registers[3]=10;handler.sp-=0x100;handler.registers[1]=handler.sp;
 check(a.stage_handler(ai,fa,handler,cap())&&!a.ready(ai,fa,cap()),"stage handler only from existing ready origin");fa=handler;
 check(a.publish_handler(ai,fa,cap())&&a.ready(ai,fa,cap())&&!a.complete_syscall(ai,fa,cap(),{0,0}),"handler commit preserves resumability without completion origin");
 check(a.diagnostics(ai,fa,cap(),da)&&da.event==p::Event::timer,"handler retains trusted diagnostic ancestry");
 handler=fa;handler.registers[5]^=16;check(a.stage_handler(ai,fa,handler,cap()),"stage exact handler candidate");fa=handler;fa.registers[6]^=1;
 check(!a.publish_handler(ai,fa,cap())&&!a.ready(ai,fa,cap()),"post-stage extra handler edit rejected");
 fa=seed();fa.registers[0]=172;capture(a,ai,fa,ah.value(),8,n::TrapKind::syscall);auto returned=seed(0x3456);returned.pc+=0x200;
 check(a.stage_sigreturn(ai,fa,returned,cap())&&!a.complete_syscall(ai,fa,cap(),{0,0}),"actual sigreturn stages before commit and blocks completion");fa=returned;
 check(a.publish_sigreturn(ai,fa,cap())&&a.ready(ai,fa,cap())&&!a.publish_linux_completion(ai,fa,cap(),{0,0}),"sigreturn restores full state without normal result clobber");
 check(p::same_frame(fa,returned),"all32 SPE upper halves ACC SPEFSCR and controls survive sigreturn commit");
 fa=seed();capture(a,ai,fa,ah.value(),8,n::TrapKind::syscall);check(!a.stage_sigreturn(ai,fa,returned,cap())&&!a.stage_handler(ai,fa,handler,cap()),"non-return syscall and pending handler fail closed");
 fa.registers[0]=2;capture(a,ai,fa,ah.value(),8,n::TrapKind::syscall);fc=fa;check(R::Bridge::encode_return(fc,{0,0}).ok(),"child result staged");
 auto corrupt=fc;corrupt.fp_state[127]^=1;check(!c.inherit_syscall(ci,a,ai,fa,corrupt,{},cap()),"child cannot alter retained SPE state");
 check(c.inherit_syscall(ci,a,ai,fa,fc,{},cap())&&c.ready(ci,fc,cap()),"fork child derived from actual parent pending syscall");
 check(!c.complete_syscall(ci,fc,cap(),{0,0})&&!c.stage_sigreturn(ci,fc,returned,cap()),"derived child cannot claim actual syscall completion");
 fg=fc;check(!g.inherit_syscall(gi,c,ci,fc,fg,{},cap()),"derived origin cannot mint another child");
 check(c.reset(ci,image,cl,cap())&&!c.inherit_syscall(ci,a,ai,fa,fc,{},cap()),"same identity reset cannot rearm child publication");
 ci.image_generation++;check(c.reset(ci,image,cl,cap()),"new child image identity");fa=seed();fa.registers[0]=120;capture(a,ai,fa,ah.value(),8,n::TrapKind::syscall);
 check(a.complete_syscall(ai,fa,cap(),{co.tid,0}),"parent clone result completed first");fc=fa;R::ChildEdits edits{0x7ffe0000,0x4000f000};R::Bridge::set_stack_pointer(fc,*edits.stack);check(R::Bridge::set_thread_pointer(fc,{*edits.tls}).ok()&&R::Bridge::encode_return(fc,{0,0}).ok(),"declared clone stack TLS result staging");
 check(c.inherit_syscall(ci,a,ai,fa,fc,edits,cap())&&c.ready(ci,fc,cap()),"clone child preserves full state except declared edits");
 check(!c.publish_linux_completion(ci,fc,cap(),{0,0}),"derived completed parent still grants no child syscall origin");
 auto expected_child=fc;for(usize i=0;i<32;++i)for(usize j=0;j<4;++j)check(fc.fp_state[i*8+j]==fa.fp_state[i*8+j],"every SPE upper byte retained by clone");
 for(usize i=256;i<268;++i)check(fc.fp_state[i]==fa.fp_state[i],"ACC SPEFSCR retained by clone");
 c.revoke();check(!c.ready(ci,fc,cap())&&!c.stage_relocation(ci,fc,cap()),"explicit pre-retirement revocation");
 check(pool.release(ch.value(),co).ok()&&!cl.inactive(),"released lease token invalid");auto reused=pool.reserve(co);check(reused&&reused.value().generation!=ch.value().generation,"same owner slot reuse has new generation");check(!c.ready(ci,expected_child,cap()),"reuse never inherits source origin");
 fa=seed();capture(a,ai,fa,ah.value(),6,n::TrapKind::exception,0x08000000,0x60000000);check(a.ready(ai,fa,cap())&&a.diagnostics(ai,fa,cap(),da)&&da.raw_srr1==0x0208c000&&da.dsisr==0x08000000,"program SRR1 cause proof persists");
 Access::enter(tracker);check(a.ready(ai,fa,cap()),"unrelated entry preserves program diagnostic origin");
 fa=seed();capture(a,ai,fa,ah.value(),14,n::TrapKind::page_fault,0xdeadbeef,0x60000000);check(a.diagnostics(ai,fa,cap(),da)&&da.dar==fa.pc&&!da.dsisr&&da.raw_esr==0xdeadbeef,"instruction fault Linux normalization retains raw ESR");
 Access::qualify(tracker);check(!a.ready(ai,fa,cap())&&!b.ready(bi,fb,cap()),"CPU reinitialization revokes every persisted owner");
 auto fresh_image=Image::inspect(cap(),elf,elf.size(),user::ppc32_elf_admission::Personality::linux_user,user::ppc32_elf_admission::RuntimeProfile::e500v2_scalar_soft_float);ai.image_generation++;check(a.reset(ai,fresh_image,al,cap()),"new epoch rebind requires current image admission");
 fa=seed();capture(a,ai,fa,ah.value(),10,n::TrapKind::timer);arena[0]^=std::byte{1};check(!a.ready(ai,fa,cap())&&pool.accounting().quarantined_slots==1,"damaged inactive canary quarantines and revokes proof");
 std::printf("PPC_PERSISTENT_MODEL_PASS checks=%zu scope=hosted-lifecycle actual_guest=0\n",checks);
}
