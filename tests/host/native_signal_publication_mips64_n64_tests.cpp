#define OK_MIPS64_N64_ADMISSION_MODEL 1
#include "ok/arch/mips64/signal.hpp"
#include "ok/user/native_mips64_n64_elf_admission.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <iterator>
using namespace ok;
namespace hw=ok::arch::execution;
namespace cpu=hw::mips64_n64_private;
using Ready=ok::user::native::Mips64N64SignalReadiness;
namespace ok::arch::execution::mips64_n64_private::detail {
struct ModelAccess {Tracker value{};};
}
static unsigned checks;
#define REQUIRE(x) do {++checks;if(!(x)){std::fprintf(stderr,"N64_REVIEW_FAIL line=%d check=%u\n",__LINE__,checks);std::exit(1);}}while(false)
static cpu::Facts measured()
{
    cpu::Facts f;
    f.prid=0x10000;f.config0=0x8000c482;f.config1=0xfea3519b;f.config2=0x80000000;f.config3=0x80;
    f.has_config1=f.has_config2=f.has_config3=true;f.status=0x24000080;f.fir=0x7f0000;
    return f;
}
alignas(4096) static std::array<std::byte,memory::KernelEntryStackPool::arena_bytes> backing;
static void lifecycle()
{
    cpu::detail::ModelAccess issuer;
    auto &backend=issuer.value;
    REQUIRE(backend.qualify(measured()));
    memory::KernelEntryStackPool pool;
    REQUIRE(pool.activate(backing));
    const memory::KernelEntryStackOwner owner{71,91};
    const auto reservation=pool.reserve(owner);REQUIRE(reservation);
    const auto lease=reservation.value();const auto stack=pool.top(lease,owner);REQUIRE(stack);
    hw::AddressSpace space{0x333000,3,17};
    Ready::Identity id{reinterpret_cast<uptr>(&pool),stack.value()-memory::KernelEntryStackPool::usable_bytes,stack.value(),lease.slot,owner.pid,owner.tid,lease.generation,23,space};
    Ready ready;
    hw::UserFrame f;f.pc=0x40004000;f.sp=0x4000f000;f.registers[29]=f.sp;f.flags=0x240080b3;
    f.fs_base=0x76543210;f.registers[16]=0x1234567887654321ULL;
    ready.reset(id,backend.capability());REQUIRE(ready.publish_initial(id,f,backend.capability()));
    REQUIRE(!ready.publish_syscall_completion(id,f,backend.capability()));
    REQUIRE(!ready.publish_sigreturn(id,f,backend.capability()));
    for(unsigned cycle=0;cycle!=12;++cycle)
    {
        const auto active=pool.activate_entry(lease,owner,0);REQUIRE(active);auto view=active.value();
        REQUIRE(ready.begin_entry(id,f,space,view,backend.capability()));
        REQUIRE(!ready.ready(id,f,backend.capability()));
        backend.invalidate();
        const bool timer=cycle%3!=0, delay=cycle%3==2;
        const hw::Trap t{timer?hw::TrapKind::timer:hw::TrapKind::syscall,0,delay?0x80008000ULL:0x8000ULL,0,view.top()-832};
        backend.publish_owned(f,space,view,t);auto proof=backend.query(f);
        REQUIRE(proof.valid_for(f,space,view));
        auto moved=f;REQUIRE(!proof.valid_for(moved,space,view));
        auto wrong=space;++wrong.slot;REQUIRE(!proof.valid_for(f,wrong,view));
        const auto pc=f.pc;f.pc^=4;REQUIRE(!proof.valid_for(f,space,view));f.pc=pc;
        REQUIRE(ready.capture(id,f,space,view,proof,backend.capability()));
        REQUIRE(pool.deactivate_entry(lease,owner,0));
        REQUIRE(!proof.valid_for(f,space,view));
        REQUIRE(ready.ready(id,f,backend.capability())==!delay);
        REQUIRE(ready.deferred(id,f,backend.capability())==delay);
        if(delay) {
            REQUIRE(!ready.publish_handler(id,f,backend.capability()));
            REQUIRE(!ready.publish_sigreturn(id,f,backend.capability()));
        } else if(!timer) {
            auto original=f;
            f.registers[2]=0x5555666677778888ULL;f.registers[7]=0;
            REQUIRE(ready.publish_syscall_completion(id,f,backend.capability()));
            f.registers[3]^=1;REQUIRE(!ready.publish_syscall_completion(id,f,backend.capability()));f.registers[3]^=1;
            f.fs_base^=8;REQUIRE(!ready.publish_syscall_completion(id,f,backend.capability()));f.fs_base^=8;
            // An inherited child may resume, but cannot attest that it trapped.
            Ready child;auto child_id=id;++child_id.tid;++child_id.image_generation;
            auto child_frame=f;child_frame.registers[2]=0;
            child.reset(child_id,backend.capability());
            REQUIRE(child.inherit_syscall(child_id,ready,id,f,child_frame,backend.capability()));
            REQUIRE(!child.publish_syscall_completion(child_id,child_frame,backend.capability()));
            REQUIRE(!child.publish_sigreturn(child_id,child_frame,backend.capability()));
            f=original;REQUIRE(ready.publish_sigreturn(id,f,backend.capability()));
            REQUIRE(!ready.publish_syscall_completion(id,f,backend.capability()));
        }
        auto alien=id;++alien.lease_generation;REQUIRE(!ready.ready(alien,f,backend.capability()));
    }
    // A capture minted before this begin must not satisfy a later entry.
    auto active=pool.activate_entry(lease,owner,0);REQUIRE(active);auto v=active.value();
    backend.invalidate();backend.publish_owned(f,space,v,{hw::TrapKind::syscall,0,0,0,v.top()-832});
    auto stale=backend.query(f);REQUIRE(ready.begin_entry(id,f,space,v,backend.capability()));
    REQUIRE(!ready.capture(id,f,space,v,stale,backend.capability()));
    REQUIRE(pool.deactivate_entry(lease,owner,0));
    ready.revoke(id);REQUIRE(!ready.ready(id,f,backend.capability()));
    ready.reset(id,backend.capability());REQUIRE(!ready.publish_initial(id,f,backend.capability()));
    ++id.image_generation;ready.reset(id,backend.capability());REQUIRE(ready.publish_initial(id,f,backend.capability()));
    auto before=backend.capability();auto bad=measured();bad.fcsr=1;REQUIRE(!backend.qualify(bad));
    REQUIRE(!before.admitted());REQUIRE(!ready.ready(id,f,backend.capability()));
    REQUIRE(pool.release(lease,owner));
    // Explicit reviewed branch encodings: k0 and link-destroyed operands reject.
    for(u32 instruction:std::array<u32,5>{0x03400008,0x03600008,0x03e0f809,0x07f10001,0x17400001})
        REQUIRE(!cpu::delay_operands_admitted(instruction));
    for(u32 instruction:std::array<u32,5>{0x03e00008,0x0200f809,0x11090001,0x45010001,0x08001000})
        REQUIRE(cpu::delay_operands_admitted(instruction));
}

static void effects()
{
    const auto first_check=checks;
    cpu::detail::ModelAccess issuer;auto &backend=issuer.value;REQUIRE(backend.qualify(measured()));
    memory::KernelEntryStackPool pool;REQUIRE(pool.activate(backing));
    const memory::KernelEntryStackOwner owner{1,2};auto reserved=pool.reserve(owner);REQUIRE(reserved);auto lease=reserved.value();
    auto top=pool.top(lease,owner);REQUIRE(top);hw::AddressSpace space{0x123000,4,31};
    Ready::Identity id{reinterpret_cast<uptr>(&pool),top.value()-memory::KernelEntryStackPool::usable_bytes,top.value(),lease.slot,1,2,lease.generation,1,space};
    for(u64 number:std::array<u64,3>{5000,5021,5242})
        for(i64 result:std::array<i64,3>{0,4,-22})
            for(unsigned edits=0;edits!=8;++edits)
            {
                Ready ready;ready.reset(id,backend.capability());
                hw::UserFrame f;f.pc=0x40001000;f.sp=0x40004000;f.registers[29]=f.sp;f.flags=0x240080b3;
                f.registers[2]=number;f.registers[4]=0xfedcba9876543210ULL;f.fs_base=0x40007000;
                auto activated=pool.activate_entry(lease,owner,0);REQUIRE(activated);auto view=activated.value();
                REQUIRE(ready.begin_entry(id,f,space,view,backend.capability()));backend.invalidate();
                backend.publish_owned(f,space,view,{hw::TrapKind::syscall,0,0,0,view.top()-832});
                REQUIRE(ready.capture(id,f,space,view,backend.query(f),backend.capability()));REQUIRE(pool.deactivate_entry(lease,owner,0));
                f.registers[2]=result<0?static_cast<u64>(-result):static_cast<u64>(result);f.registers[7]=result<0?1:0;
                if(edits&1)f.fs_base=f.registers[4];
                if(edits&2)f.registers[3]=42;
                if(edits&4)f.pc+=4;
                bool valid=!(edits&4);
                valid=valid && (((edits&1)!=0)==(number==5242&&result==0));
                if(edits&2)valid=valid&&number==5021&&result>=0;
                REQUIRE(ready.publish_linux_completion(id,f,backend.capability(),result)==valid);
                REQUIRE(ready.ready(id,f,backend.capability())==valid);
            }
    REQUIRE(pool.release(lease,owner));
    std::printf("N64_COMPLETION_EFFECTS_PASS new_checks=%u source=author-model not-execution=1\n",checks-first_check);
}
static void publication()
{
    const auto first=checks;
    cpu::detail::ModelAccess issuer;auto &backend=issuer.value;REQUIRE(backend.qualify(measured()));
    memory::KernelEntryStackPool pool;REQUIRE(pool.activate(backing));
    const memory::KernelEntryStackOwner owner{5,7};auto r=pool.reserve(owner);REQUIRE(r);auto lease=r.value();
    auto top=pool.top(lease,owner);REQUIRE(top);hw::AddressSpace space{0x553000,4,9};
    Ready::Identity base{reinterpret_cast<uptr>(&pool),top.value()-memory::KernelEntryStackPool::usable_bytes,top.value(),lease.slot,5,7,lease.generation,4,space};
    for(unsigned kind=0;kind<7;++kind)
    {
        auto id=base;
        Ready ready;hw::UserFrame f;f.pc=0x40001000;f.sp=0x40004000;f.registers[29]=f.sp;f.flags=0x240080b3;
        f.registers[2]=5000;f.fs_base=0xfedcba9876543210ULL;
        ready.reset(id,backend.capability());REQUIRE(ready.publish_initial(id,f,backend.capability()));
        if(kind)
        {
            auto active=pool.activate_entry(lease,owner,0);REQUIRE(active);auto view=active.value();
            REQUIRE(ready.begin_entry(id,f,space,view,backend.capability()));backend.invalidate();
            const bool timer=kind==2||kind==3;
            backend.publish_owned(f,space,view,{timer?hw::TrapKind::timer:hw::TrapKind::syscall,0,kind==3?0x80000000ULL:0,0,view.top()-832});
            REQUIRE(ready.capture(id,f,space,view,backend.query(f),backend.capability()));
            REQUIRE(pool.deactivate_entry(lease,owner,0));
            if(kind==4)REQUIRE(ready.publish_handler(id,f,backend.capability()));
            if(kind==5)REQUIRE(ready.publish_sigreturn(id,f,backend.capability()));
            if(kind==6)
            {
                Ready child;auto child_id=id;++child_id.tid;auto copy=f;
                child.reset(child_id,backend.capability());
                REQUIRE(child.inherit_syscall(child_id,ready,id,f,copy,backend.capability()));
                REQUIRE(child.stage_relocation(child_id,copy,backend.capability()));
                REQUIRE(child.commit_relocation(child_id,f,backend.capability()));
                ready=child;id=child_id;
            }
        }
        const auto state=ready.state();
        auto moved=f;
        REQUIRE(!ready.ready(id,moved,backend.capability()));
        REQUIRE(!ready.commit_relocation(id,moved,backend.capability()));
        REQUIRE(ready.stage_relocation(id,f,backend.capability()));
        REQUIRE(!ready.stage_relocation(id,f,backend.capability()));
        REQUIRE(ready.commit_relocation(id,moved,backend.capability()));
        REQUIRE(ready.state()==state);
        REQUIRE(!ready.commit_relocation(id,f,backend.capability()));
        REQUIRE(!ready.ready(id,f,backend.capability()));
        REQUIRE(ready.ready(id,moved,backend.capability())==(kind!=3));
        REQUIRE(ready.deferred(id,moved,backend.capability())==(kind==3));
        REQUIRE(ready.stage_relocation(id,moved,backend.capability()));
        auto corrupted=moved;corrupted.fs_base^=1;
        REQUIRE(!ready.commit_relocation(id,corrupted,backend.capability()));
        REQUIRE(!ready.commit_relocation(id,f,backend.capability()));
        REQUIRE(ready.stage_relocation(id,moved,backend.capability()));
        auto alien=id;++alien.tid;
        REQUIRE(!ready.commit_relocation(alien,f,backend.capability()));
        REQUIRE(!ready.commit_relocation(id,f,backend.capability()));
        REQUIRE(ready.stage_relocation(id,moved,backend.capability()));
        ready.cancel_relocation();REQUIRE(!ready.commit_relocation(id,f,backend.capability()));
        auto next=id;++next.space.generation;next.space.slot=2;next.space.root=0x777000;
        REQUIRE(ready.can_publish_space(id,moved,next.space,backend.capability()));
        REQUIRE(!ready.can_publish_space(id,moved,{},backend.capability()));
        REQUIRE(!ready.can_publish_space(id,moved,id.space,backend.capability()));
        REQUIRE(!ready.can_publish_space(id,corrupted,next.space,backend.capability()));
        auto wrong=next;++wrong.image_generation;REQUIRE(!ready.publish_space(id,wrong,moved,backend.capability()));
        wrong=next;++wrong.lease_generation;REQUIRE(!ready.publish_space(id,wrong,moved,backend.capability()));
        REQUIRE(ready.state()==state);REQUIRE(ready.identity()==id);
        // A different task's entry or retirement advances the hardware issuer,
        // but does not erase this task's already consumed parked provenance.
        backend.invalidate();REQUIRE(ready.publish_space(id,next,moved,backend.capability()));
        REQUIRE(ready.state()==state);REQUIRE(ready.identity()==next);
        REQUIRE(!ready.ready(id,moved,backend.capability()));
        REQUIRE(!ready.publish_space(id,next,moved,backend.capability()));
        REQUIRE(ready.ready(next,moved,backend.capability())==(kind!=3));
        REQUIRE(ready.deferred(next,moved,backend.capability())==(kind==3));
        auto completed=moved;
        moved.registers[2]=0;
        const bool actual=kind==1;
        REQUIRE(ready.publish_syscall_completion(next,moved,backend.capability())==actual);
        if(!actual)moved=completed;
        REQUIRE(ready.stage_relocation(next,moved,backend.capability()));
        ready.revoke(next);
        REQUIRE(!ready.commit_relocation(next,f,backend.capability()));
        REQUIRE(!ready.publish_space(next,id,moved,backend.capability()));
        REQUIRE(!ready.publish_initial(next,moved,backend.capability()));
    }
    REQUIRE(pool.release(lease,owner));
    std::printf("N64_PUBLICATION_PASS author_new_checks=%u execution=0\n",checks-first);
}
int main(){lifecycle();const auto inherited=checks;effects();const auto effects_count=checks-inherited;publication();std::printf("N64_LIFECYCLE_PASS total=%u repeated_review=%u repeated_author_effects=%u\n",checks,inherited,effects_count);}
