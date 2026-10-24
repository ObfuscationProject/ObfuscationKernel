// Hosted backend observations and copy stubs; no native execution evidence.
#define OK_MIPS64_N64_ADMISSION_MODEL 1
#define OK_PPC32_ADMISSION_MODEL 1
#include "../../src/user/native_syscall_restart_publication.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <memory>
using namespace ok;
namespace hw = arch::execution;
namespace native = user::native;
namespace n64 = hw::mips64_n64_private;
namespace ppc = hw::ppc32_private;
using Profile = abi::linux_user::Profile;
static usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"PUBLICATION_FAIL line=%d checks=%zu\n",__LINE__,checks);std::abort(); } } while(false)
namespace ok::arch::execution::mips64_n64_private::detail {
struct ModelAccess { Tracker tracker{}; };
}
namespace ok::arch::execution::ppc32_private::detail {
struct ModelAccess {
    static Tracker make() { return Tracker{}; }
    static void qualify(Tracker &tracker) {tracker.qualify_initialized({0x80210022,0,0,true});}
    static void capture(Tracker &tracker,const UserFrame &frame,AddressSpace space,
                        const KernelEntryStackView &view,const Trap &trap)
    { tracker.invalidate();tracker.publish_owned(frame,space,view,trap); }
};
}
static n64::detail::ModelAccess n64_issuer;
static auto ppc_issuer = ppc::detail::ModelAccess::make();
static Profile selected;
static bool delay_slot{}, code_access=true;
static hw::AddressSpace live_space{0x1000,1,7};
static constexpr uptr site=hw::user_begin+0x1000;
static constexpr uptr output=hw::user_begin+0x4000;
static std::vector<std::byte> ppc_elf;
alignas(4096) static std::array<std::byte,memory::KernelEntryStackPool::arena_bytes> arena;
bool hw::ppc32_private::address_space_live(hw::AddressSpace space) noexcept
{ return ppc::same_space(space,live_space); }
bool hw::user_range(const AddressSpace &space,uptr address,usize bytes,bool)
{ return code_access && n64::same_space(space,live_space) && address>=site && address+bytes<=site+4; }
Status hw::copy_from_user(const AddressSpace &,void *target,uptr address,usize bytes)
{
    if (!code_access || address!=site || bytes!=4) return Status::fault("hosted opcode span");
    const std::array<u8,4> opcode = selected==Profile::mips64 ? std::array<u8,4>{0,0,0,12}
                                                          : std::array<u8,4>{0x44,0,0,2};
    std::memcpy(target,opcode.data(),bytes); return Status::success();
}
Result<hw::Trap> hw::enter_owned(AddressSpace &space,UserFrame &frame,const KernelEntryStackView &view)
{
    CHECK(view.valid());
    hw::Trap trap{hw::TrapKind::syscall,8,selected==Profile::mips64?u64{32} | (delay_slot?0x80000000ULL:0):0,
                  0,view.top()-(selected==Profile::mips64?sizeof(hw::UserFrame)+16:ppc::entry_extent)};
    if(selected==Profile::mips64)
    { n64_issuer.tracker.invalidate(); n64_issuer.tracker.publish_owned(frame,space,view,trap); }
    else { ppc::detail::ModelAccess::capture(ppc_issuer,frame,space,view,trap); }
    return trap;
}

template <Profile P> struct Fixture
{
    static constexpr bool is_n64 = P==Profile::mips64;
    using Pub=native::SyscallRestartPublication<P>;
    using R=typename Pub::Readiness;
    using Origin=typename Pub::Origin;
    using Identity=typename Pub::Identity;
    memory::KernelEntryStackPool pool;
    Origin origins{pool};
    sched::Scheduler scheduler;
    sched::ProcessControlBlock &process;
    i32 fd{};
    u16 description{},pipe{};
    static sched::ProcessControlBlock &make(sched::Scheduler &scheduler)
    {
        const auto result=scheduler.create_user_process("restart-publication",{});CHECK(result && result.value()==1);
        return *scheduler.find(result.value());
    }
    R readiness;
    Identity id{};
    typename Origin::Identity owner{};
    typename Origin::PipeWriteBinding binding{};
    hw::UserFrame original{};
    sched::ThreadControlBlock &thread(){return process.threads()[0];}
    auto cap() const {if constexpr(is_n64)return n64_issuer.tracker.capability();else return ppc_issuer.capability();}
    Fixture() : process(make(scheduler))
    {
        selected=P;delay_slot=false;code_access=true;
        CHECK(pool.activate(arena));
        auto &t=thread();t.owner=1;t.tid=1;t.native_active=true;
        auto lease=pool.reserve({1,1});CHECK(lease);t.entry_stack=lease.value();
        const auto top=pool.top(lease.value(),{1,1});CHECK(top);t.kernel_stack_top=top.value();
        auto &resources=process.resources();resources.native_space=live_space;
        resources.personality=sched::UserPersonality::linux;
        const auto pair=scheduler.create_pipe(resources,false,0);CHECK(pair);
        fd=pair.value()[1];description=resources.files.entry(fd)->description;
        pipe=scheduler.open_description(description)->pipe;CHECK(scheduler.retain_description(description));
        CHECK(resources.native_pages.push_back({site,0x2000,hw::page_read|hw::page_execute}));
        CHECK(origins.prepare(t));
        if constexpr(is_n64)
        {
            CHECK(n64_issuer.tracker.qualify({0x10000,0x8000c482,0xfea3519b,0x80000000,0x80,0,0,
                0x24000080,0x007f0000,0,true,true,true,false,false}));
            // This independent image counter intentionally differs from origin.image.
            id={reinterpret_cast<uptr>(&pool),top.value()-memory::KernelEntryStackPool::usable_bytes,
                top.value(),lease.value().slot,1,1,lease.value().generation,101,live_space};
            readiness.reset(id,cap());
        }
        else
        {
            ppc::detail::ModelAccess::qualify(ppc_issuer);CHECK(ppc_issuer.capability().admitted());
            auto image=native::Ppc32ImageAdmission::inspect(cap(),ppc_elf,ppc_elf.size(),
                user::ppc32_elf_admission::Personality::linux_user,
                user::ppc32_elf_admission::RuntimeProfile::e500v2_scalar_soft_float);
            id={live_space,1,1,101};
            CHECK(readiness.reset(id,image,native::Ppc32OwnedLease::inspect(pool,lease.value(),{1,1}),cap()));
        }
    }
    void capture(bool so=false)
    {
        selected=P;
        auto &t=thread();t.syscall_pending=false;
        auto &f=t.native_frame;f={};
        for(usize i=0;i<f.registers.size();++i)f.registers[i]=0x11220000+i*17;
        for(usize i=0;i<f.fp_state.size();++i)f.fp_state[i]=static_cast<u8>(i*19+7);
        f.pc=site+4;f.sp=hw::user_begin+0x9000;f.fs_base=hw::user_begin+0x8000;f.gs_base=hw::user_begin+0x7000;
        if constexpr(is_n64)
        {
            f.flags=n64::user_status;f.registers[2]=5001;f.registers[4]=fd;f.registers[5]=output;f.registers[6]=3;
            f.registers[7]=0xfedcba9876543210ULL;f.registers[29]=f.sp;
            for(usize i=256;i<260;++i)f.fp_state[i]=0;
        }
        else
        {
            f.flags=ppc::user_msr;f.registers[0]=4;f.registers[3]=fd;f.registers[4]=output;f.registers[5]=3;
            f.registers[1]=f.sp;f.registers[2]=f.fs_base;f.registers[13]=f.gs_base;
            for(usize i=264;i<268;++i)f.fp_state[i]=i==267?2:0;
            f.fp_state[280]=so?0x15:5;
        }
        const auto view=pool.activate_entry(t.entry_stack,{1,1},0);CHECK(view);
        CHECK(readiness.begin_entry(id,f,live_space,view.value(),cap()));
        CHECK(origins.enter(process,t,view.value(),scheduler));
        if constexpr(is_n64)
            CHECK(readiness.capture(id,f,live_space,view.value(),n64_issuer.tracker.query(f),cap()));
        else CHECK(readiness.capture(id,f,live_space,view.value(),ppc_issuer.query(f),cap()));
        CHECK(pool.deactivate_entry(t.entry_stack,{1,1},0));
        owner=origins.identity(process,t);original=f;
        t.syscall_pending=true;
        const auto previous=t.wait_registration.ticket();
        if(t.wait_registration.pending(previous))CHECK(t.wait_registration.complete(previous,sched::WaitCompletion::cancelled));
        auto ticket=t.wait_registration.arm(1,1);CHECK(ticket);
        binding={{&origins,1,1,ticket.value().generation},ticket.value(),ticket.value().generation,fd,output,3,description,pipe};
        t.state=sched::ProcessState::blocked;t.wait_reason=sched::ThreadWaitReason::pipe_write;t.wait_key=pipe;
        if(!delay_slot && code_access)CHECK(origins.bind(owner,f,binding));
    }
    auto stage(){return Pub::stage(origins,owner,binding,readiness,id,thread().native_frame,cap());}
    bool commit(typename Pub::template BasicTransaction<typename Origin::PipeWriteBinding> &tx)
    {return Pub::commit(tx,owner,binding,id,thread().native_frame,cap());}
    bool ready()const{return readiness.ready(id,process.threads()[0].native_frame,cap());}
    void normal_refused()
    {
        if constexpr(is_n64) CHECK(!readiness.publish_linux_completion(id,thread().native_frame,cap(),0));
        else CHECK(!readiness.complete_syscall(id,thread().native_frame,cap(),{0,0}));
    }
};

template<Profile P> void run()
{
    using F=Fixture<P>;using Pub=typename F::Pub;
    auto storage=std::make_unique<F>();auto &f=*storage;
    for(bool so:{false,true})
    {
        f.capture(so);
        auto staged=f.stage();CHECK(staged);auto replay=staged.value();
        CHECK(!f.ready());f.normal_refused();
        auto &frame=f.thread().native_frame;
        CHECK(Pub::Origin::same_frame(frame,f.original));
        CHECK(!f.readiness.stage_relocation(f.id,frame,f.cap()));
        CHECK(f.commit(staged.value()));CHECK(f.ready());f.normal_refused();
        CHECK(frame.pc==site);
        CHECK(!f.origins.live(f.owner));
        // A different live frame must not merely hide an unconsumed record.
        // Restoring identical old bytes cannot revive capture/wait authority.
        const auto committed=frame;frame=f.original;
        CHECK(!f.origins.prepare_restart(f.owner,frame,f.binding));
        frame=committed;CHECK(f.ready());
        auto expected=f.original;expected.pc=site;
        if constexpr(P==Profile::ppc)expected.fp_state[280]|=0x10;
        CHECK(Pub::Origin::same_frame(frame,expected));
        auto context=f.readiness.context(f.id,frame,f.cap());
        if constexpr(P==Profile::mips64)
        {
            CHECK(context.resume==native::Mips64N64SignalFrame::ResumeContract::prepared_syscall_restart);
            CHECK(native::Mips64N64SignalFrame::capture(frame,context));
            CHECK(frame.registers[2]==5001 && frame.registers[7]==0xfedcba9876543210ULL);
        }
        else
        {
            CHECK(context.resume==native::Ppc32SignalContext::Resume::prepared_syscall_restart);
            CHECK(context.metadata.result==0 && context.metadata.original_r3==static_cast<u32>(f.fd) && context.metadata.trap==0xc00);
            CHECK(frame.registers[3]==static_cast<uptr>(f.fd) && (frame.fp_state[280]&0x10));
            const auto legacy=context.template codec<native::Ppc32SignalFormat::legacy>();
            const auto realtime=context.template codec<native::Ppc32SignalFormat::realtime>();
            CHECK(native::Ppc32LegacySignalFrame::supported(frame,legacy.contract));
            CHECK(native::Ppc32RtSignalFrame::supported(frame,realtime.contract));
        }
        CHECK(!f.commit(replay));CHECK(f.ready());Pub::cancel(replay);CHECK(f.ready());
        auto handler=frame;handler.pc=site+0x100;handler.sp-=0x400;
        if constexpr(P==Profile::ppc)
        {
            handler.registers[1]=handler.sp;
            CHECK(f.readiness.stage_handler(f.id,frame,handler,f.cap()));frame=handler;
        }
        else {handler.registers[29]=handler.sp;frame=handler;}
        CHECK(f.readiness.publish_handler(f.id,frame,f.cap()));CHECK(f.ready());
    }
    // Every retained scalar/control/FP byte is part of the prepublication seal.
    for(usize field=0;field<32+5+512;++field)
    {
        f.capture();auto staged=f.stage();CHECK(staged);auto &frame=f.thread().native_frame;
        if(field<32)frame.registers[field]^=1;
        else if(field<37)
        {
            const std::array<uptr hw::UserFrame::*,5> fields{&hw::UserFrame::pc,&hw::UserFrame::sp,
                &hw::UserFrame::flags,&hw::UserFrame::fs_base,&hw::UserFrame::gs_base};frame.*fields[field-32]^=1;
        }
        else frame.fp_state[field-37]^=1;
        const auto changed=frame;CHECK(!f.commit(staged.value()));
        CHECK(Pub::Origin::same_frame(frame,changed));CHECK(!f.ready());
    }
    for(unsigned field=0;field<12;++field)
    {
        f.capture();auto owner=f.owner;auto binding=f.binding;
        switch(field)
        {
            case 0:++owner.pid;break;case 1:++owner.tid;break;case 2:++owner.image;break;
            case 3:++owner.capture;break;case 4:++owner.epoch;break;case 5:++owner.lease.generation;break;
            case 6:++owner.space.generation;break;case 7:++binding.ticket.generation;break;
            case 8:++binding.request;break;case 9:++binding.input;break;case 10:++binding.description;break;
            default:++binding.object.generation;break;
        }
        CHECK(!Pub::stage(f.origins,owner,binding,f.readiness,f.id,f.thread().native_frame,f.cap()));
        auto staged=f.stage();CHECK(staged);
        CHECK(!Pub::commit(staged.value(),owner,binding,f.id,f.thread().native_frame,f.cap()));
        CHECK(!f.ready());CHECK(Pub::Origin::same_frame(f.thread().native_frame,f.original));
    }
    f.capture();auto staged=f.stage();CHECK(staged);auto stale=staged.value();Pub::cancel(staged.value());CHECK(!f.ready());
    f.capture();auto fresh=f.stage();CHECK(fresh);Pub::cancel(stale);CHECK(f.commit(fresh.value()));CHECK(f.ready());
    f.capture();staged=f.stage();CHECK(staged);f.origins.revoke(f.thread());CHECK(!f.commit(staged.value()));CHECK(!f.ready());
    f.capture();staged=f.stage();CHECK(staged);
    auto after=f.binding;
    CHECK(f.thread().wait_registration.complete(f.binding.ticket,sched::WaitCompletion::ready));
    auto next=f.thread().wait_registration.arm(1,1);CHECK(next);after.ticket=next.value();
    CHECK(f.origins.rearm(f.owner,f.thread().native_frame,f.binding,after));
    CHECK(!f.commit(staged.value()));CHECK(!f.ready());
    f.capture();staged=f.stage();CHECK(staged);
    auto v=f.pool.activate_entry(f.thread().entry_stack,{1,1},0);CHECK(v);
    CHECK(f.pool.deactivate_entry(f.thread().entry_stack,{1,1},0));
    CHECK(!f.commit(staged.value()));CHECK(!f.ready());
    code_access=false;f.capture();CHECK(!f.stage());code_access=true;
    if constexpr(P==Profile::mips64){delay_slot=true;f.capture();CHECK(!f.stage());delay_slot=false;}
    f.origins.retire(f.thread());
    CHECK(f.pool.release(f.thread().entry_stack,{1,1}));
    std::printf("WRITE_RESTART_PUBLICATION_PROFILE_PASS profile=%s checks=%zu hosted=1 guest=0\n",P==Profile::mips64?"mips64":"ppc",checks);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);std::ifstream in(argv[1],std::ios::binary);CHECK(in.good());
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),{});ppc_elf.resize(bytes.size());
    std::memcpy(ppc_elf.data(),bytes.data(),bytes.size());
    run<Profile::mips64>();run<Profile::ppc>();
    std::printf("WRITE_RESTART_PUBLICATION_PASS profiles=2 checks=%zu hosted=1 guest=0\n",checks);
}
