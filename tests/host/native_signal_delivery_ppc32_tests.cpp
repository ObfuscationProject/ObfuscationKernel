#include "ok/arch/ppc/signal.hpp"
#include "../fixtures/signals/ppc32_trampoline_baseline.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::user::native;
using Profile = abi::linux_user::Profile;
using Delivery = Ppc32SignalDelivery;
using Return = Ppc32SignalReturn;
using Frame = arch::execution::UserFrame;
using Queue = sched::PendingSignals<64>;
using Context = Ppc32SignalContext;
using Mask = sched::SignalMask;
using Stack = sched::SignalAltstack;
using Actions = sched::SignalActionTable;
constexpr u64 begin=0x40000000, end=0x80000000, base=0x60000000, handler=begin+0x1000, trampoline=begin+0x2000;
static usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line=%d check=%zu %s\n",__LINE__,checks,#x); std::abort(); } } while (0)
static bool equal(const Frame &a,const Frame &b) { return a.registers==b.registers && a.pc==b.pc && a.sp==b.sp && a.flags==b.flags && a.fs_base==b.fs_base && a.gs_base==b.gs_base && a.fp_state==b.fp_state; }
static constexpr Context context{Context::Hardware::native32_be_e500v2_eager_spe_only,Context::Resume::normalized_no_restart,{0x12345678,0xc00,0x6000ff00,0xabcd1234,0xfffffff2},true};
static bool size_valid(u64) noexcept { return true; }
static bool admit(const Frame &) { return true; }
static Frame sample()
{
    using C=Ppc32LegacySignalFrame;
    Frame f;f.pc=begin+0x4000;f.sp=base+0x7000;f.flags=0x0200c000;f.fs_base=0x61234560;f.gs_base=0x62345670;
    for(usize i=0;i<32;++i)f.registers[i]=0x11002200+i;
    f.fp_state.fill(0xa6);
    for(usize i=0;i<32;++i){C::backend_put(f,8*i,0xabcdef00+i,4);C::backend_put(f,8*i+4,0x77777777,4);}
    C::backend_put(f,256,0x123456789abcdef0,8);C::backend_put(f,264,0xff3fff7f,4);
    C::backend_put(f,272,begin+0x1234,4);C::backend_put(f,276,0xabc00123,4);C::backend_put(f,280,0x91000100,4);C::backend_put(f,284,0xe0000011,4);
    return f;
}
struct Memory
{
    std::array<std::byte,0x10000> bytes{};
    usize reads{},writes{},fail_read{},limit{};bool fail_write{};
    u64 write_address{};usize write_bytes{};
    Memory(){bytes.fill(std::byte{0xcd});}
    bool valid(u64 p,usize n)const{return p>=base&&p-base<=bytes.size()&&n<=bytes.size()-(p-base);}
    bool read(u64 p,std::span<std::byte> dst){++reads;if(!valid(p,dst.size()))return false;const usize n=reads==fail_read?limit:dst.size();for(usize i=0;i<n;++i)dst[i]=bytes[p-base+i];return reads!=fail_read;}
    bool write(u64 p,std::span<const std::byte> src){++writes;write_address=p;write_bytes=src.size();if(!valid(p,src.size()))return false;const usize n=fail_write?limit:src.size();for(usize i=0;i<n;++i)bytes[p-base+i]=src[i];return !fail_write;}
    u64 get(u64 p,usize n=4)const{u64 v{};for(usize i=0;i<n;++i)v=(v<<8)|static_cast<u8>(bytes[p-base+i]);return v;}
    void put(u64 p,u64 v,usize n=4){for(usize i=0;i<n;++i)bytes[p-base+n-1-i]=static_cast<std::byte>(v>>(8*i));}
};
struct Fixture
{
    Queue queue;Actions actions;Mask blocked;Frame frame=sample();Stack stack{base+0xa000,0x3000,0};Memory memory;u32 selected{};
    Fixture(){CHECK(queue.reset({1,2,3}));blocked.set(33);blocked.set(64);}
    void setup(u64 flags=0,u32 signal=10){*actions.find(signal)={handler,flags,0xdeadbeef,{}};actions.find(signal)->mask.set(12);CHECK(queue.enqueue({signal,-6,0x1234,0x5678})==Queue::Enqueue::queued);}
    Delivery::Result deliver(const Context &c=context,u64 tr=trampoline,bool entry=true){return Delivery::deliver(queue,actions,blocked,frame,stack,begin,end,c,[&](u64 p,std::span<const std::byte>s){return memory.write(p,s);},selected,tr,[&](const Frame&){return entry;});}
};
template<Profile P>static void prior_trampoline(){constexpr auto old=baseline::SignalTrampoline<P>::image();constexpr auto now=SignalTrampoline<P>::image();static_assert(old==now);CHECK(old==now);}
static void trampoline_test()
{
    prior_trampoline<Profile::rv32>();prior_trampoline<Profile::rv64>();prior_trampoline<Profile::mips>();prior_trampoline<Profile::mips64>();prior_trampoline<Profile::loongarch64>();
    using T=SignalTrampoline<Profile::ppc>;constexpr auto bytes=T::image();constexpr std::array<u32,8> expected{0x380000ac,0x44000002,0x7fe00008,0x7fe00008,0x38000077,0x44000002,0x7fe00008,0x7fe00008};
    for(usize i=0;i<8;++i)for(usize j=0;j<4;++j)CHECK(static_cast<u8>(bytes[4*i+j])==static_cast<u8>(expected[i]>>(24-8*j)));
    u64 address=77;CHECK(T::entry_address(end-32,SignalTrampolineEntry::legacy,begin,end,address));CHECK(address==end-16);
    for(u64 p:{end-28,end-16,end-4,end,begin-4,trampoline+1}){address=77;CHECK(!T::entry_address(p,SignalTrampolineEntry::realtime,begin,end,address));CHECK(address==77);}
    CHECK(T::entry_offset(SignalTrampolineEntry::legacy)==16);CHECK(T::entry_offset(SignalTrampolineEntry::realtime)==0);
}
template<Ppc32SignalFormat Format>static void delivery_test()
{
    using C=Ppc32SignalFrame<Format>;constexpr u64 rt=C::realtime?4:0;
    for(unsigned flags=0;flags<32;++flags)
    {
        Fixture f;const u64 options=rt|((flags&1)?Delivery::nodefer:0)|((flags&2)?Delivery::resethand:0)|((flags&4)?Delivery::onstack:0)|((flags&16)?Delivery::restorer:0);
        if(flags&8)f.stack.flags=Stack::autodisarm;
        f.setup(options);const Frame initial=f.frame;const auto stack=f.stack;const auto blocked=f.blocked;
        CHECK(f.deliver()==Delivery::Result::handler);CHECK(!f.queue.pending_mask().contains(10));
        const u64 top=(flags&4)?stack.base+stack.size:initial.sp;
        const u64 fp=(top-(C::realtime?1536:1248))&~u64{15};const u64 sp=fp-(C::realtime?80:64);
        CHECK(f.frame.sp==sp&&f.frame.registers[1]==sp&&f.frame.pc==handler&&f.frame.registers[3]==10&&f.frame.registers[4]==fp);
        CHECK(C::backend_get(f.frame,272,4)==trampoline+(C::realtime?0:16));
        CHECK(f.frame.fs_base==initial.fs_base&&f.frame.gs_base==initial.gs_base&&f.frame.registers[2]==initial.fs_base&&f.frame.registers[13]==initial.gs_base);
        CHECK(C::backend_get(f.frame,264,4)==0xff3fff7f);CHECK(C::backend_get(f.frame,256,8)==C::backend_get(initial,256,8));
        for(usize i=0;i<32;++i)CHECK(C::backend_get(f.frame,8*i,4)==C::backend_get(initial,8*i,4));
        CHECK(f.memory.write_address==sp&&f.memory.write_bytes==(C::realtime?1392:1088));CHECK(f.memory.get(sp)==initial.sp);
        for(usize i=4;i<C::handler_gap;++i)CHECK(f.memory.get(sp+i,1)==0);
        for(usize i=0;i<224;++i)CHECK(f.memory.get(sp+C::image_bytes+i,1)==0xcd);
        CHECK(f.blocked.contains(33)&&f.blocked.contains(64)&&f.blocked.contains(12)&&f.blocked.contains(10)==!(flags&1));
        CHECK(f.actions.find(10)->handler==((flags&2)?0:handler));CHECK(f.stack==((flags&8)?Stack{}:stack));
        if constexpr(C::realtime){CHECK(f.frame.registers[5]==fp+128&&f.frame.registers[6]==fp);CHECK(f.memory.get(fp)==10&&f.memory.get(fp+8)==0xfffffffa);CHECK(f.memory.get(fp+136)==stack.base&&f.memory.get(fp+140)==stack.flags&&f.memory.get(fp+144)==stack.size);}
        else{CHECK(f.memory.get(fp+12)==(blocked.words[0]>>32)&&f.memory.get(fp+24)==(blocked.words[0]&0xffffffff));CHECK(f.frame.registers[5]==initial.registers[5]&&f.frame.registers[6]==initial.registers[6]);}
        CHECK(f.memory.get(sp+C::context_offset+34*4)==context.metadata.original_r3);CHECK(f.memory.get(sp+C::context_offset+40*4)==context.metadata.trap);
    }
    for(usize cutoff=0;cutoff<=C::image_bytes;++cutoff)
    {
        Fixture f;f.stack.flags=Stack::autodisarm;f.setup(rt|Delivery::resethand|Delivery::onstack);const auto frame=f.frame;const auto actions=f.actions;const auto stack=f.stack;const auto blocked=f.blocked;Queue::Delivery ticket;CHECK(f.queue.peek({},ticket));
        f.memory.fail_write=true;f.memory.limit=cutoff;CHECK(f.deliver()==Delivery::Result::terminate);CHECK(f.selected==11&&f.memory.writes==1);CHECK(equal(f.frame,frame)&&f.actions==actions&&f.stack==stack&&f.blocked==blocked);Queue::Delivery after;CHECK(f.queue.peek({},after)&&after==ticket);
    }
    for(unsigned kind=0;kind<9;++kind)
    {
        Fixture f;f.setup(rt|Delivery::resethand|Delivery::onstack);Context c=context;u64 tr=trampoline;bool entry=true;
        if(kind==0)c={};
        if(kind==1)c.metadata_supplied=false;
        if(kind==2)c.resume=Context::Resume::unverified;
        if(kind==3)c.hardware=Context::Hardware::unverified;
        if(kind==4)f.frame.flags|=0x2000;
        if(kind==5)tr=end-16;
        if(kind==6)f.stack.size=100;
        if(kind==7)entry=false;
        if(kind==8)f.stack={end-10,100,0};
        const auto frame=f.frame;const auto actions=f.actions;const auto stack=f.stack;const auto mask=f.blocked;
        CHECK(f.deliver(c,tr,entry)==Delivery::Result::terminate);CHECK(equal(f.frame,frame)&&f.actions==actions&&f.stack==stack&&f.blocked==mask&&f.queue.pending_mask().contains(10));CHECK(f.memory.writes==(kind==7?1:0));
    }
    for(u64 bad_sp:{u64{0},u64{1},u64{0xffffffff}}){Fixture f;f.frame.sp=bad_sp;f.setup(rt|Delivery::onstack);CHECK(f.deliver()==Delivery::Result::handler);CHECK(f.memory.get(f.frame.sp)==bad_sp);}
    {Fixture f;f.frame.sp=f.stack.base+16;f.setup(rt);CHECK(f.deliver()==Delivery::Result::terminate);CHECK(f.memory.writes==0);}
    {Fixture f;f.frame.sp=f.stack.base+0x2000;const auto top=f.frame.sp;f.setup(rt|Delivery::onstack);CHECK(f.deliver()==Delivery::Result::handler);CHECK(f.frame.sp==top-C::allocation_bytes);}
}
template<Ppc32SignalFormat Format>static void return_test()
{
    using C=Ppc32SignalFrame<Format>;constexpr auto op=C::realtime?Return::Operation::rt_sigreturn:Return::Operation::sigreturn;
    Fixture saved;saved.setup((C::realtime?4:0)|Delivery::onstack);const auto original=saved.frame;const auto prior_mask=saved.blocked;CHECK(saved.deliver()==Delivery::Result::handler);
    const auto entered=saved.frame;const auto raw=saved.memory;const u64 sp=entered.sp;const u64 mc=sp+C::context_offset;
    const auto restore=[&](Memory &memory,Frame &frame,Stack &stack,Mask &mask,Context::Metadata &metadata,const Context &ctx=context,bool entry=true){return Return::restore(op,mask,frame,stack,metadata,ctx,begin,end,[&](u64 p,std::span<std::byte>d){return memory.read(p,d);},[&](const Frame&){return entry;},size_valid);};
    for(usize read_index=1;read_index<=2;++read_index)for(usize cutoff=0;cutoff<=(read_index==1?C::prefix_bytes:992);++cutoff)
    {
        auto memory=raw;memory.fail_read=read_index;memory.limit=cutoff;auto frame=entered;Stack stack{base+0x1000,0x2000,0};const auto initial_stack=stack;Mask mask;mask.set(3);const auto initial_mask=mask;Context::Metadata metadata{1,2,3,4,5};const auto initial_metadata=metadata;
        CHECK(!restore(memory,frame,stack,mask,metadata));CHECK(equal(frame,entered)&&stack==initial_stack&&mask==initial_mask&&metadata==initial_metadata);CHECK(memory.reads==read_index);
    }
    for(unsigned variant=0;variant<7;++variant)
    {
        auto memory=raw;auto frame=entered;Stack stack{};Mask mask{};Context::Metadata metadata{};
        const u64 target=variant==1?base+0x1000:mc;
        if(variant==1){for(usize i=0;i<992;++i)memory.bytes[target-base+i]=raw.bytes[mc-base+i];memory.put(sp+C::handler_gap+(C::realtime?176:28),target);}
        memory.put(target,0xfedcba98);memory.put(target+3*4,0xabcdef01);memory.put(target+38*4,0xf1234567);memory.put(target+2*4,0x61234000);memory.put(target+13*4,0x67890000);
        memory.put(target+34*4,0x01020304);memory.put(target+40*4,0x500);memory.put(target+41*4,0xdeadbeef);memory.put(target+42*4,0x12345678);memory.put(target+43*4,0xfefefefe);
        if(variant==2)memory.put(target+33*4,0);
        if(variant==3)memory.put(target+600,0);
        if constexpr(C::realtime){if(variant==4)memory.put(sp+80+140,0x40);if(variant==5)memory.put(sp+80+144,1);if(variant==6)stack={original.sp-0x100,0x1000,0};}
        const auto old_stack=stack;
        if constexpr(C::realtime)memory.put(sp+80+180,memory.get(sp+80+180)|(1u<<8)|(1u<<18));
        else memory.put(sp+64+24,memory.get(sp+64+24)|(1u<<8)|(1u<<18));
        const usize writes=memory.writes;const auto before_bytes=memory.bytes;CHECK(restore(memory,frame,stack,mask,metadata));CHECK(memory.reads==2&&memory.writes==writes&&memory.bytes==before_bytes);
        CHECK(frame.registers[0]==0xfedcba98&&frame.registers[3]==0xabcdef01&&C::backend_get(frame,280,4)==0xf1234567);CHECK(frame.fs_base==0x61234000&&frame.gs_base==0x67890000&&frame.registers[1]==frame.sp&&frame.sp==original.sp);
        CHECK(C::backend_get(frame,264,4)==(variant==3?0:0xff3fff7f));CHECK(frame.flags==entered.flags&&C::backend_get(frame,288,4)==C::backend_get(entered,288,4));
        for(usize i=0;i<32;++i){CHECK(C::backend_get(frame,8*i+4,4)==frame.registers[i]);CHECK(C::backend_get(frame,8*i,4)==(variant==2?0:C::backend_get(original,8*i,4)));}
        CHECK(C::backend_get(frame,256,8)==(variant==2?0:C::backend_get(original,256,8)));CHECK(mask==prior_mask);CHECK(metadata.original_r3==0x01020304&&metadata.trap==0x501&&metadata.dar==0xdeadbeef&&metadata.dsisr==0x12345678&&metadata.result==0xfefefefe);
        if constexpr(C::realtime)CHECK(stack==((variant>=4)?old_stack:saved.stack));else CHECK(stack==old_stack);
    }
    for(unsigned bad=0;bad<9;++bad)
    {
        auto memory=raw;auto frame=entered;Stack stack{};Mask mask{};Context::Metadata metadata{1,2,3,4,5};Context ctx=context;
        if(bad==0)memory.put(mc+128,0);
        if(bad==1)memory.put(mc+4,0);
        if(bad==2)memory.put(sp+C::handler_gap+(C::realtime?176:28),base+1);
        if(bad==3)memory.put(mc+132,0x02002000);
        if(bad==4)memory.put(mc+600,0x80);
        if(bad==5)memory.put(mc+192,1);
        if(bad==6)ctx={};
        if(bad==8)ctx.metadata_supplied=false;
        CHECK(!restore(memory,frame,stack,mask,metadata,ctx,bad!=7));CHECK(equal(frame,entered)&&stack==Stack{}&&mask==Mask{}&&(metadata==Context::Metadata{1,2,3,4,5}));
    }
    auto memory=raw;auto frame=entered;Return::Restored staged;staged.frame=sample();const auto untouched=staged.frame;
    CHECK(!Return::decode(Return::Operation::getpid,frame,context,begin,end,[&](u64 p,std::span<std::byte>b){return memory.read(p,b);},admit,staged));CHECK(memory.reads==0&&equal(staged.frame,untouched));
    CHECK(!Return::decode(op,frame,context,begin,end,[&](u64 p,std::span<std::byte>b){return memory.read(p,b);},[](const Frame&){return false;},staged));CHECK(memory.reads==2&&equal(staged.frame,untouched)&&staged.mask==Mask{}&&staged.stack==Stack{}&&staged.metadata==Context::Metadata{});
}
static void queues()
{
    {Fixture f;const auto frame=f.frame;CHECK(f.deliver()==Delivery::Result::none);CHECK(equal(frame,f.frame)&&f.memory.writes==0);}
    {Fixture f;f.setup();f.blocked.set(10);CHECK(f.deliver()==Delivery::Result::none&&f.queue.pending_mask().contains(10));}
    for(u32 signal:{17u,18u,23u,28u}){Fixture f;CHECK(f.queue.enqueue({signal,0,1,2})==Queue::Enqueue::queued);CHECK(f.deliver()==Delivery::Result::ignored&&f.memory.writes==0);}
    {Fixture f;f.setup();f.actions.find(10)->handler=1;CHECK(f.deliver()==Delivery::Result::ignored&&!f.queue.pending_mask().contains(10));}
    for(u32 signal:{19u,20u,21u,22u}){Fixture f;CHECK(f.queue.enqueue({signal,0,1,2})==Queue::Enqueue::queued);CHECK(f.deliver()==Delivery::Result::unsupported&&f.queue.pending_mask().contains(signal));}
    {Fixture f;CHECK(f.queue.enqueue({64,0,1,2})==Queue::Enqueue::queued);f.blocked.set(64,false);CHECK(f.deliver()==Delivery::Result::terminate&&f.selected==64);CHECK(f.queue.enqueue({65,0,1,2})==Queue::Enqueue::invalid);}
    {Fixture f;f.setup(4,64);f.blocked.set(64,false);CHECK(f.deliver()==Delivery::Result::handler&&f.frame.registers[3]==64&&f.blocked.contains(64));}
    {Fixture f;f.setup(1);CHECK(f.deliver()==Delivery::Result::unsupported&&f.memory.writes==0&&f.queue.pending_mask().contains(10));}
    {Fixture f;f.setup(4);CHECK(f.queue.enqueue({10,0,99,100})==Queue::Enqueue::coalesced);CHECK(f.deliver()==Delivery::Result::handler);CHECK(f.memory.get(f.frame.sp+80+12)==0x1234&&f.memory.get(f.frame.sp+80+16)==0x5678);}
    {Fixture f;*f.actions.find(8)={handler,4,0,{}};CHECK(f.queue.enqueue_kernel(8)==Queue::Enqueue::queued);CHECK(f.deliver()==Delivery::Result::terminate&&f.selected==11&&f.queue.pending_mask().contains(8)&&f.memory.writes==0);}
    {
        struct FailingCommit
        {
            using Delivery=Queue::Delivery;
            Queue queue;
            bool peek(const Mask &mask,Delivery &ticket)const{return queue.peek(mask,ticket);}
            bool consume(const Delivery &){return false;}
        } pending;
        CHECK(pending.queue.reset({1,2,3}));CHECK(pending.queue.enqueue({10,0,1,2})==Queue::Enqueue::queued);
        Fixture f;f.stack.flags=Stack::autodisarm;*f.actions.find(10)={handler,4|Delivery::resethand|Delivery::onstack,0,{}};
        const auto original=f.frame;const auto actions=f.actions;const auto mask=f.blocked;const auto stack=f.stack;bool admitted=false;
        CHECK(Delivery::deliver(pending,f.actions,f.blocked,f.frame,f.stack,begin,end,context,
            [&](u64 p,std::span<const std::byte>s){CHECK(equal(f.frame,original)&&f.actions==actions&&f.blocked==mask&&f.stack==stack);return f.memory.write(p,s);},
            f.selected,trampoline,[&](const Frame&){CHECK(f.memory.writes==1&&equal(f.frame,original)&&f.actions==actions&&f.blocked==mask&&f.stack==stack);admitted=true;return true;})==Delivery::Result::corrupt);
        CHECK(admitted&&pending.queue.pending_mask().contains(10)&&equal(f.frame,original)&&f.actions==actions&&f.blocked==mask&&f.stack==stack);
    }
    {
        Fixture f;f.setup(Delivery::nodefer);f.actions.find(10)->mask.set(10);f.actions.find(10)->mask.set(9);f.actions.find(10)->mask.set(19);f.actions.find(10)->mask.set(100);
        CHECK(f.deliver()==Delivery::Result::handler);CHECK(f.blocked.contains(10)&&!f.blocked.contains(9)&&!f.blocked.contains(19)&&!f.blocked.contains(100));
    }
}
int main(){trampoline_test();delivery_test<Ppc32SignalFormat::legacy>();delivery_test<Ppc32SignalFormat::realtime>();return_test<Ppc32SignalFormat::legacy>();return_test<Ppc32SignalFormat::realtime>();queues();std::printf("PPC32_POLICY_PASS checks=%zu\n",checks);}
