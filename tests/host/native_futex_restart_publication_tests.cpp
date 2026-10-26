// Futex typed publication using retained pipe-suite observation fixtures; no native execution.
#define OK_MIPS64_N64_ADMISSION_MODEL 1
#define OK_PPC32_ADMISSION_MODEL 1
#include "../../src/user/native_syscall_restart_publication.hpp"
#include "native_restart_model_syscall.hpp"
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
    alignas(4096) std::array<std::byte,4096> mapping_arena{};
    memory::FrameAllocator mapping_frames;
    memory::FrameMetadataStorage<2048> mapping_frames_metadata;
    native::NativePagePool mapping_pool;
    sched::Scheduler scheduler;
    sched::ProcessControlBlock &process;
    i32 fd{};
    u16 description{},pipe{};
    static sched::ProcessControlBlock &make(sched::Scheduler &scheduler)
    {
        const auto result=scheduler.create_init_process({.mode=arch::PrivilegeMode::user});CHECK(result && result.value()==1);
        return *scheduler.find(result.value());
    }
    R readiness;
    Identity id{};
    typename Origin::Identity owner{};
    typename Origin::FutexBinding binding{};
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
        const memory::MemoryRegion mapping_region{reinterpret_cast<uptr>(mapping_arena.data()),mapping_arena.size(),memory::RegionType::usable};
        CHECK(mapping_frames.initialize({&mapping_region,1}, 4096, mapping_frames_metadata.bytes));
        CHECK(mapping_pool.activate(mapping_frames,native::NativePagePool::Source::linker_reserved,1));
        CHECK(resources.native_pages.bind(mapping_pool));
        resources.personality=sched::UserPersonality::linux;
        const auto pair=scheduler.create_pipe(resources,false,0);CHECK(pair);
        fd=pair.value()[0];description=resources.files.entry(fd)->description;
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
    void capture(bool timed=false)
    {
        selected=P;
        auto &t=thread();t.syscall_pending=false;
        auto &f=t.native_frame;f={};
        for(usize i=0;i<f.registers.size();++i)f.registers[i]=0x11220000+i*17;
        for(usize i=0;i<f.fp_state.size();++i)f.fp_state[i]=static_cast<u8>(i*19+7);
        f.pc=site+4;f.sp=hw::user_begin+0x9000;f.fs_base=hw::user_begin+0x8000;f.gs_base=hw::user_begin+0x7000;
        if constexpr(is_n64)
        {
            f.flags=n64::user_status;f.registers[2]=5000;f.registers[4]=fd;f.registers[5]=output;f.registers[6]=3;
            f.registers[7]=0xfedcba9876543210ULL;f.registers[29]=f.sp;
            for(usize i=256;i<260;++i)f.fp_state[i]=0;
        }
        else
        {
            f.flags=ppc::user_msr;f.registers[0]=3;f.registers[3]=fd;f.registers[4]=output;f.registers[5]=3;
            f.registers[1]=f.sp;f.registers[2]=f.fs_base;f.registers[13]=f.gs_base;
            for(usize i=264;i<268;++i)f.fp_state[i]=i==267?2:0;
            f.fp_state[280]=5;
        }
        if constexpr(is_n64)
        { f.registers[2]=5194;f.registers[4]=output;f.registers[5]=128;f.registers[6]=7;f.registers[7]=timed?output+64:0; }
        else
        { f.registers[0]=221;f.registers[3]=output;f.registers[4]=128;f.registers[5]=7;f.registers[6]=timed?output+64:0; }
        CHECK(scheduler.set_runnable(1));
        CHECK(scheduler.activate_thread(1,1));
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
        binding={{&origins,1,1,ticket.value().generation},ticket.value(),output,128,7,is_n64?5194u:221u,timed?output+64:0,timed?1000u:0u,timed?1u:0u};
        t.state=sched::ProcessState::blocked;t.wait_reason=sched::ThreadWaitReason::futex;t.wait_key=output;
        if(!delay_slot && code_access)CHECK(origins.bind(owner,f,binding));
    }
    auto stage(){return Pub::stage(origins,owner,binding,readiness,id,thread().native_frame,cap());}
    bool commit(typename Pub::FutexTransaction &tx)
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
    static_assert(!std::same_as<typename Pub::Transaction,typename Pub::FutexTransaction>);
    auto storage=std::make_unique<F>();auto &f=*storage;
    f.capture(true);CHECK(f.origins.validate_bound(f.owner,f.thread().native_frame,f.binding));CHECK(!f.stage());
    CHECK(Pub::Origin::same_frame(f.thread().native_frame,f.original));
    f.capture();auto staged=f.stage();CHECK(staged);auto duplicate=staged.value();
    CHECK(f.commit(staged.value()));CHECK(f.ready());CHECK(f.thread().native_frame.pc==site);
    CHECK(!f.commit(duplicate));CHECK(f.ready());CHECK(!f.origins.live(f.owner));
    for(unsigned field=0;field<10;++field)
    {
        f.capture();auto wrong=f.binding;
        switch(field){case 0:++wrong.address;break;case 1:wrong.operation=0;break;case 2:++wrong.expected;break;case 3:++wrong.syscall;break;case 4:++wrong.timeout_address;break;case 5:++wrong.deadline;break;case 6:++wrong.deadline_generation;break;case 7:++wrong.ticket.generation;break;case 8:++wrong.object.generation;break;default:wrong.object.pool=nullptr;}
        CHECK(!Pub::stage(f.origins,f.owner,wrong,f.readiness,f.id,f.thread().native_frame,f.cap()));
        auto tx=f.stage();CHECK(tx);CHECK(!Pub::commit(tx.value(),f.owner,wrong,f.id,f.thread().native_frame,f.cap()));
        CHECK(!f.ready());CHECK(Pub::Origin::same_frame(f.thread().native_frame,f.original));
    }
    f.capture();auto tx=f.stage();CHECK(tx);
    CHECK(f.thread().wait_registration.complete(f.binding.ticket,sched::WaitCompletion::ready));
    CHECK(!f.commit(tx.value()));CHECK(!f.ready());
    f.capture();tx=f.stage();CHECK(tx);auto stale=tx.value();Pub::cancel(tx.value());
    f.capture();auto fresh=f.stage();CHECK(fresh);Pub::cancel(stale);CHECK(f.commit(fresh.value()));CHECK(f.ready());
    f.capture();tx=f.stage();CHECK(tx);auto active=f.pool.activate_entry(f.thread().entry_stack,{1,1},0);CHECK(active);CHECK(f.pool.deactivate_entry(f.thread().entry_stack,{1,1},0));CHECK(!f.commit(tx.value()));CHECK(!f.ready());
    f.origins.retire(f.thread());CHECK(f.pool.release(f.thread().entry_stack,{1,1}));
    std::printf("FUTEX_PUBLICATION_PROFILE_PASS profile=%s checks=%zu hosted=1 native=0\n",P==Profile::mips64?"mips64":"ppc",checks);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);std::ifstream in(argv[1],std::ios::binary);CHECK(in.good());
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),{});ppc_elf.resize(bytes.size());std::memcpy(ppc_elf.data(),bytes.data(),bytes.size());
    run<Profile::mips64>();run<Profile::ppc>();
    std::printf("FUTEX_PUBLICATION_PASS profiles=2 checks=%zu hosted=1 native=0\n",checks);
}
