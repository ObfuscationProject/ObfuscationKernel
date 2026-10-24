// Typed origin authority model with real scheduler/entry pool; no native execution.
#include "../../src/user/native_syscall_origin.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
using namespace ok;
namespace hw = arch::execution;
namespace native = user::native;
namespace lu = abi::linux_user;
using Profile = lu::Profile;
static usize checks{}, copies{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FUTEX_ORIGIN_FAIL line=%d checks=%zu\n",__LINE__,checks); std::abort(); } } while(false)
static constexpr uptr site=hw::user_begin+4096, word=hw::user_begin+128;
static Profile selected;
static hw::Trap observed;
alignas(4096) static std::array<std::byte,memory::KernelEntryStackPool::arena_bytes> arena;
bool hw::user_range(const AddressSpace &,uptr address,usize size,bool)
{ return address==site && size<=4; }
Status hw::copy_from_user(const AddressSpace &,void *out,uptr address,usize size)
{
    ++copies;CHECK(address==site && size<=4);
    std::array<u8,4> opcode{};
    switch(selected) {
    case Profile::i386:opcode={0xcd,0x80};break;
    case Profile::x86_64:opcode={0x0f,0x05};break;
    case Profile::aarch64:opcode={1,0,0,0xd4};break;
    case Profile::arm32:opcode={0,0,0,0xef};break;
    case Profile::rv32:case Profile::rv64:opcode={0x73,0,0,0};break;
    case Profile::loongarch64:opcode={0,0,0x2b,0};break;
    case Profile::mips:case Profile::mips64:opcode={0,0,0,12};break;
    case Profile::ppc:opcode={0x44,0,0,2};break;
    default:std::abort();
    }
    std::memcpy(out,opcode.data(),size);return Status::success();
}
Result<hw::Trap> hw::enter_owned(AddressSpace &,UserFrame &,const KernelEntryStackView &view)
{ auto trap=observed;trap.kernel_stack_pointer=view.lower();return trap; }

template<Profile P> hw::UserFrame frame(u64 number,u32 operation,bool timed)
{
    hw::UserFrame f{};f.pc=site+4;f.sp=hw::user_begin+0x9000;
    std::array<usize,4> slots{};usize nr{};
    if constexpr(P==Profile::x86_64){nr=14;slots={9,10,11,5};f.pc=site+2;observed={hw::TrapKind::syscall,256};}
    else if constexpr(P==Profile::i386){nr=7;slots={4,6,5,1};f.pc=site+2;observed={hw::TrapKind::syscall,128};}
    else if constexpr(P==Profile::aarch64){nr=8;slots={0,1,2,3};observed={hw::TrapKind::syscall,0x15,0x56000000};}
    else if constexpr(P==Profile::arm32){nr=7;slots={0,1,2,3};f.flags=0x10;f.registers[15]=f.pc;observed={hw::TrapKind::syscall,2};}
    else if constexpr(P==Profile::rv32 || P==Profile::rv64){nr=17;slots={10,11,12,13};observed={hw::TrapKind::syscall,8,8};}
    else if constexpr(P==Profile::loongarch64){nr=11;slots={4,5,6,7};observed={hw::TrapKind::syscall,11};}
    else if constexpr(P==Profile::mips || P==Profile::mips64){nr=2;slots={4,5,6,7};observed={hw::TrapKind::syscall,8,32};if constexpr(P==Profile::mips)f.sp=0;}
    else {nr=0;slots={3,4,5,6};observed={hw::TrapKind::syscall,8};}
    f.registers[nr]=number;const std::array<u64,4> args{word,operation,7,timed?word+16:0};
    for(usize i=0;i<4;++i)f.registers[slots[i]]=args[i];
    if constexpr(lu::Codec<P>::profile.word_bytes==8) {f.registers[slots[1]]|=u64{1}<<32;f.registers[slots[2]]|=u64{1}<<32;}
    return f;
}
template<Profile P> struct Fixture
{
    using Origin=native::SyscallOriginTable<P>;
    memory::KernelEntryStackPool pool;
    Origin origins{pool};
    sched::Scheduler scheduler;
    sched::ProcessControlBlock *process{};
    Fixture(){CHECK(pool.activate(arena));auto made=scheduler.create_user_process("futex-origin",{});CHECK(made);process=scheduler.find(made.value());
        auto &t=thread();t.native_active=true;auto lease=pool.reserve({process->pid(),t.tid});CHECK(lease);t.entry_stack=lease.value();
        process->resources().personality=sched::UserPersonality::linux;process->resources().native_space={0x1000,1,1};
        CHECK(process->resources().native_pages.push_back({site,0x2000,hw::page_read|hw::page_execute}));CHECK(origins.prepare(t));}
    auto &thread(){return process->threads()[0];}
    ~Fixture(){origins.retire(thread());CHECK(pool.release(thread().entry_stack,{process->pid(),thread().tid}));}
    auto capture(u64 number,u32 operation,bool timed){selected=P;auto &t=thread();t.syscall_pending=false;t.native_frame=frame<P>(number,operation,timed);
        const auto before=copies;auto active=pool.activate_entry(t.entry_stack,{process->pid(),t.tid},0);CHECK(active);
        CHECK(origins.enter(*process,t,active.value(),scheduler));CHECK(pool.deactivate_entry(t.entry_stack,{process->pid(),t.tid},0));
        CHECK(copies==before+1);return origins.identity(*process,t);}
};
template<Profile P> void run()
{
    using F=Fixture<P>;using Origin=typename F::Origin;
    for(auto operation:{u32{0},u32{128}}) for(bool timed:{false,true})
        for(auto call:{lu::Operation::futex,lu::Operation::futex_time64}) {
            const auto number=lu::Codec<P>::syscall_number(call);if(!number)continue;
            auto f=std::make_unique<F>();auto &t=f->thread();auto id=f->capture(*number,operation,timed);CHECK(id.capture);
            auto ticket=t.wait_registration.arm(f->process->pid(),t.tid);CHECK(ticket);t.syscall_pending=true;
            t.state=sched::ProcessState::blocked;t.wait_reason=sched::ThreadWaitReason::futex;t.wait_key=word;
            typename Origin::FutexBinding binding{{&f->origins,1,1,1},ticket.value(),word,operation,7,*number,timed?word+16:0,timed?1000u:0u,timed?1u:0u};
            auto changed=binding;++changed.expected;CHECK(!f->origins.bind(id,t.native_frame,changed));
            CHECK(f->origins.bind(id,t.native_frame,binding));CHECK(f->origins.validate_bound(id,t.native_frame,binding));
            CHECK(!f->origins.validate_bound(id,t.native_frame,typename Origin::PipeBinding{}));
            for(unsigned field=0;field<10;++field){changed=binding;switch(field){case 0:++changed.address;break;case 1:changed.operation^=128;break;case 2:++changed.expected;break;case 3:++changed.syscall;break;case 4:++changed.timeout_address;break;case 5:++changed.deadline;break;case 6:++changed.deadline_generation;break;case 7:++changed.ticket.generation;break;case 8:++changed.object.generation;break;default:changed.object.pool=nullptr;}
                CHECK(!f->origins.validate_bound(id,t.native_frame,changed));}
            auto permit=f->origins.prepare_restart(id,t.native_frame,binding);CHECK(permit.ok()==!timed);
            if(!timed){CHECK(permit.value().plan().frame.pc==site);CHECK(f->origins.consume_restart(permit.value(),id,t.native_frame,binding));CHECK(!f->origins.consume_restart(permit.value(),id,t.native_frame,binding));}
            else {const auto previous=f->process->resources().native_space;auto next=previous;++next.generation;next.root+=4096;
                auto update=f->origins.stage_space(&f->scheduler,f->process->resources(),previous,next);CHECK(update);f->process->resources().native_space=next;CHECK(f->origins.commit_space(update.value(),&f->scheduler,f->process->resources()));id.space=next;
                CHECK(f->origins.validate_bound(id,t.native_frame,binding));auto view=f->pool.activate_entry(t.entry_stack,{f->process->pid(),t.tid},0);CHECK(view);CHECK(f->pool.deactivate_entry(t.entry_stack,{f->process->pid(),t.tid},0));CHECK(!f->origins.validate_bound(id,t.native_frame,binding));}
        }
    {auto f=std::make_unique<F>();auto id=f->capture(*lu::Codec<P>::syscall_number(lu::Operation::futex),128,false);auto &t=f->thread();auto ticket=t.wait_registration.arm(f->process->pid(),t.tid);CHECK(ticket);t.syscall_pending=true;t.state=sched::ProcessState::blocked;t.wait_reason=sched::ThreadWaitReason::futex;t.wait_key=word;
        typename Origin::FutexBinding binding{{&f->origins,1,1,1},ticket.value(),word,128,7,*lu::Codec<P>::syscall_number(lu::Operation::futex),0,0,0};CHECK(f->origins.bind(id,t.native_frame,binding));CHECK(t.wait_registration.complete(ticket.value(),sched::WaitCompletion::ready));CHECK(!f->origins.validate_bound(id,t.native_frame,binding));CHECK(!f->origins.prepare_restart(id,t.native_frame,binding));}
    for(u32 unsupported:{1u,129u,3u,9u,137u,256u}) {auto f=std::make_unique<F>();CHECK(!f->capture(*lu::Codec<P>::syscall_number(lu::Operation::futex),unsupported,false).capture);}
}
int main()
{
    run<Profile::i386>();run<Profile::x86_64>();run<Profile::aarch64>();run<Profile::arm32>();run<Profile::rv64>();
    run<Profile::rv32>();run<Profile::loongarch64>();run<Profile::mips>();run<Profile::mips64>();run<Profile::ppc>();
    std::printf("FUTEX_ORIGIN_PASS profiles=10 checks=%zu hosted=1 native=0\n",checks);
}
