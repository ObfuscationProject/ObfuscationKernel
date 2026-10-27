#include "ok/arch/ppc/signal.hpp"
#include "../fixtures/signals/linux_ppc32_layout.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::user::native;
namespace o = linux_ppc32_oracle;
static usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line=%d check=%zu: %s\n",__LINE__,checks,#x); std::abort(); } } while (0)
constexpr u64 begin=0x40000000, end=0x80000000, memory_base=0x60000000, top=memory_base+0x4000;
struct Memory
{
    std::array<std::byte,0x6000> bytes{};
    usize reads{}, writes{}, fail_read{}, limit{};
    bool fail_write{};
    Memory() { bytes.fill(std::byte{0xcd}); }
    bool valid(u64 at,usize n) const { return at>=memory_base && at-memory_base<=bytes.size() && n<=bytes.size()-(at-memory_base); }
    bool read(u64 at,std::span<std::byte> out)
    {
        ++reads;
        if (!valid(at,out.size())) return false;
        const usize n=reads==fail_read ? limit : out.size();
        for(usize i=0;i<n;++i)out[i]=bytes[at-memory_base+i];
        return reads!=fail_read;
    }
    bool write(u64 at,std::span<const std::byte> in)
    {
        ++writes;
        if(!valid(at,in.size())) return false;
        const usize n=fail_write ? limit : in.size();
        for(usize i=0;i<n;++i)bytes[at-memory_base+i]=in[i];
        return !fail_write;
    }
    u64 number(u64 at,usize n=4) const
    {
        u64 value{};
        for(usize i=0;i<n;++i)value=value*256+static_cast<u8>(bytes[at-memory_base+i]);
        return value;
    }
    void number(u64 at,u64 value,usize n)
    {
        for(usize i=0;i<n;++i) { bytes[at-memory_base+n-1-i]=static_cast<std::byte>(value&255);value/=256; }
    }
};
using Frame=arch::execution::UserFrame;
static bool equal(const Frame&a,const Frame&b)
{
    return a.registers==b.registers && a.pc==b.pc && a.sp==b.sp && a.flags==b.flags &&
        a.fs_base==b.fs_base && a.gs_base==b.gs_base && a.fp_state==b.fp_state;
}
template<class C> static bool equal(const typename C::ReturnPlan&a,const typename C::ReturnPlan&b)
{
    return equal(a.frame,b.frame) && a.mask.words==b.mask.words && a.stack==b.stack &&
        a.metadata==b.metadata && a.restore_altstack==b.restore_altstack;
}
template<class C> Frame sample()
{
    Frame f;
    for(usize i=0;i<32;++i)f.registers[i]=0x11002200+i;
    f.pc=begin+0x1800;f.sp=memory_base+0x5000;f.fs_base=0x61234567;f.gs_base=0x76543210;
    f.flags=0x0200c000;f.fp_state.fill(0xa6);
    for(usize i=0;i<32;++i) { C::backend_put(f,8*i,0xab001100+i,4);C::backend_put(f,8*i+4,0x55555555,4); }
    C::backend_put(f,256,0x123456789abcdef0ULL,8);C::backend_put(f,264,0x003e007f,4);
    C::backend_put(f,272,begin+0x2004,4);C::backend_put(f,276,0x13579bdf,4);
    C::backend_put(f,280,0x91000100,4);C::backend_put(f,284,0xe000007f,4);
    C::backend_put(f,288,0x87654321,4);
    return f;
}
template<class C> void exercise()
{
    static_assert(C::context_bytes==o::mcontext_bytes && o::word_bytes==4 && o::pt_regs_bytes==176);
    static_assert(C::frame_bytes==(C::realtime ? o::rt_frame_bytes:o::legacy_frame_bytes));
    static_assert(C::handler_gap==o::handler_gap+(C::realtime?16:0));
    static_assert(C::abi_gap_bytes==o::abi_gap_bytes && C::prefix_bytes==(C::realtime?o::rt_context:o::legacy_context));
    static_assert(C::pc_slot==o::pt_nip && C::msr_slot==o::pt_msr && C::ctr_slot==o::pt_ctr && C::link_slot==o::pt_link);
    static_assert(C::xer_slot==o::pt_xer && C::cr_slot==o::pt_cr && C::fp_offset==o::mc_fp && C::spe_offset==o::mc_vregs);
    static_assert(C::spefscr_reserved==(static_cast<u32>(~o::spefscr_defined_bits)));
    static_assert(C::pad_offset==o::mc_pad && C::fp_bytes==o::classic_fp_bytes && o::spe_words==34);
    static_assert(C::uc_regs_offset==o::uc_regs && C::uc_mask_offset==o::uc_mask && C::uc_stack_offset==o::uc_stack);
    static_assert(C::image_bytes+C::abi_gap_bytes==C::allocation_bytes && C::ReturnPlan::restore_all_no_restart);
    static_assert(!C::ReturnPlan::checked_user_patch_required && !C::ReturnPlan::forced_fp_signal_required);
    const typename C::Contract contract{C::Hardware::native32_be_e500v2_eager_spe_only,C::Resume::normalized_no_restart};
    const typename C::Capture capture{contract,{0x11223344,0xc00,0x12345678,0xabcdef01,0xfffffff9},true};
    typename C::Stack stack{memory_base+0x1000,0x2000,0x80000000};
    typename C::Info info{10,-6,0x1234,0x5678};
    SignalMask mask;for(u32 n:{8,9,10,19,32,33,40,64,100})mask.set(n);
    auto frame=sample<C>();
    typename C::Placement p{1,2};
    CHECK(C::place(top,begin,end,p));
    CHECK(p.frame_address==(top-C::frame_bytes) && p.stack_pointer==top-C::allocation_bytes);
    typename C::Image image;
    CHECK(C::encode(frame,capture,mask,stack,info,p,begin+0x3000,begin,end,image));
    Memory memory;
    auto read=[&](u64 at,std::span<std::byte> dst){return memory.read(at,dst);};
    auto write=[&](u64 at,std::span<const std::byte> src){return memory.write(at,src);};
    Frame handler=frame;
    CHECK(C::prepare(frame,capture,mask,stack,info,top,begin+0x3000,begin+0x4000,begin,end,write,p,handler));
    CHECK(memory.writes==1 && memory.number(p.stack_pointer)==frame.sp);
    CHECK(handler.sp==p.stack_pointer && handler.registers[1]==p.stack_pointer && handler.pc==begin+0x3000);
    CHECK(handler.registers[2]==frame.fs_base && handler.registers[13]==frame.gs_base && handler.registers[3]==10);
    CHECK(handler.registers[4]==p.frame_address && C::backend_get(handler,272,4)==begin+0x4000);
    CHECK(handler.flags==frame.flags && C::backend_get(handler,264,4)==C::backend_get(frame,264,4));
    if constexpr(C::realtime)CHECK(handler.registers[5]==p.frame_address+128 && handler.registers[6]==p.frame_address);
    else CHECK(handler.registers[5]==frame.registers[5] && handler.registers[6]==frame.registers[6]);
    for(u64 at=top-C::abi_gap_bytes;at<top;++at)CHECK(memory.bytes[at-memory_base]==std::byte{0xcd});
    const u64 mc=p.stack_pointer+C::context_offset;
    CHECK(memory.number(mc)==frame.registers[0]);
    CHECK(memory.number(mc+4)==frame.sp && memory.number(mc+8)==frame.fs_base && memory.number(mc+52)==frame.gs_base);
    CHECK(memory.number(mc+4*o::pt_nip)==frame.pc && memory.number(mc+4*o::pt_msr)==frame.flags);
    CHECK(memory.number(mc+4*o::pt_ctr)==0x13579bdf && memory.number(mc+4*o::pt_link)==begin+0x2004);
    CHECK(memory.number(mc+4*o::pt_cr)==0x91000100 && memory.number(mc+4*o::pt_xer)==0xe000007f);
    CHECK(memory.number(mc+C::spe_acc_offset,8)==0x123456789abcdef0ULL);
    for(usize i=0;i<32;++i)CHECK(memory.number(mc+o::mc_vregs+4*i)==0xab001100+i);
    CHECK(memory.number(mc+C::spefscr_offset)==0x003e007f);
    for(usize i=o::mc_fp;i<o::mc_pad;++i)CHECK(memory.number(mc+i,1)==0);
    if constexpr(C::realtime)
    {
        CHECK(memory.number(p.frame_address)==10 && memory.number(p.frame_address+8)==0xfffffffa);
        CHECK(memory.number(p.frame_address+o::info_pid)==info.sender_pid && memory.number(p.frame_address+o::info_uid)==info.sender_uid);
        CHECK(memory.number(p.frame_address+128+o::uc_regs)==mc);
        CHECK(memory.number(p.frame_address+128+o::uc_mask)==static_cast<u32>(C::MaskCodec::sanitize(mask).words[0]));
        CHECK(memory.number(p.frame_address+128+o::uc_mask+4)==static_cast<u32>(C::MaskCodec::sanitize(mask).words[0]>>32));
    }
    else
    {
        CHECK(memory.number(p.frame_address+o::sc_regs)==mc && memory.number(p.frame_address+o::sc_handler)==begin+0x3000);
        CHECK(memory.number(p.frame_address+o::sc_mask_low)==static_cast<u32>(C::MaskCodec::sanitize(mask).words[0]));
        CHECK(memory.number(p.frame_address+o::sc_mask_high)==static_cast<u32>(C::MaskCodec::sanitize(mask).words[0]>>32));
    }
    typename C::ReturnPlan result;
    CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));
    CHECK(result.frame.pc==frame.pc && result.frame.sp==frame.sp && result.frame.flags==handler.flags);
    CHECK(result.frame.fs_base==frame.fs_base && result.frame.gs_base==frame.gs_base && result.mask.words==C::MaskCodec::sanitize(mask).words);
    CHECK(result.restore_altstack==C::realtime && result.metadata.trap==0xc01 && result.metadata.original_r3==0x11223344);
    CHECK(result.metadata.dar==0x12345678 && result.metadata.dsisr==0xabcdef01 && result.metadata.result==0xfffffff9);
    if constexpr(C::realtime)CHECK(result.stack==stack);else CHECK(result.stack==typename C::Stack{});
    for(usize i=0;i<32;++i)
    {
        CHECK(result.frame.registers[i]==C::scalar(frame,i));
        CHECK(C::backend_get(result.frame,8*i,4)==0xab001100+i);
        CHECK(C::backend_get(result.frame,8*i+4,4)==result.frame.registers[i]);
    }
    for(usize i=288;i<512;++i)CHECK(result.frame.fp_state[i]==handler.fp_state[i]);
    const Memory clean=memory;
    const auto output_sentinel=result;
    auto reject=[&]() {result=output_sentinel;memory.reads=0;CHECK(!C::decode(p.stack_pointer,handler,contract,begin,end,read,result));CHECK(equal<C>(result,output_sentinel));};
    // Full staged-copy conservation, including an independently redirected context.
    for(usize phase=1;phase<=2;++phase)for(usize count=0;count<(phase==1?C::prefix_bytes:C::context_bytes);++count)
    {
        memory=clean;memory.reads=0;memory.fail_read=phase;memory.limit=count;reject();
    }
    memory=clean;
    for(usize count=0;count<C::image_bytes;++count)
    {
        Memory partial;partial.fail_write=true;partial.limit=count;
        auto failed_write=[&](u64 at,std::span<const std::byte> bytes){return partial.write(at,bytes);};
        auto untouched=handler;typename C::Placement destination{31,47};
        CHECK(!C::prepare(frame,capture,mask,stack,info,top,begin+0x3000,begin+0x4000,begin,end,failed_write,destination,untouched));
        CHECK(equal(untouched,handler) && destination==(typename C::Placement{31,47}));
        for(u64 at=top-C::abi_gap_bytes;at<top;++at)CHECK(partial.bytes[at-memory_base]==std::byte{0xcd});
    }
    for(usize slot:{usize{0},usize{2},usize{3},usize{13},usize{31}})
    {
        memory=clean;memory.number(mc+4*slot,0x1122aa00+slot,4);
        CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));
        CHECK(result.frame.registers[slot]==0x1122aa00+slot && C::backend_get(result.frame,8*slot+4,4)==0x1122aa00+slot);
        if(slot==2)CHECK(result.frame.fs_base==0x1122aa02);
        if(slot==13)CHECK(result.frame.gs_base==0x1122aa0d);
    }
    for(usize slot:{o::pt_ctr,o::pt_link,o::pt_cr,o::pt_xer})
    {
        memory=clean;memory.number(mc+4*slot,0xabcdef10+slot,4);
        CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));
        const usize backend=slot==o::pt_ctr?276:slot==o::pt_link?272:slot==o::pt_cr?280:284;
        CHECK(C::backend_get(result.frame,backend,4)==0xabcdef10+slot);
    }
    for(usize i=0;i<32;++i)
    {
        memory=clean;memory.number(mc+o::mc_vregs+4*i,0x33445500+i,4);
        CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));
        CHECK(C::backend_get(result.frame,8*i,4)==0x33445500+i);
    }
    memory=clean;memory.number(mc+C::spe_acc_offset,0xfedcba9876543210ULL,8);
    CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));CHECK(C::backend_get(result.frame,256,8)==0xfedcba9876543210ULL);
    // Clearing saved MSR_SPE clears upper halves and ACC, not SPEFSCR or GPRs.
    memory=clean;memory.number(mc+4*o::pt_msr,0x0000c000,4);
    CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));
    for(usize i=0;i<32;++i)CHECK(C::backend_get(result.frame,8*i,4)==0);
    CHECK(C::backend_get(result.frame,256,8)==0 && C::backend_get(result.frame,264,4)==0x003e007f);
    CHECK(result.frame.flags==handler.flags && result.frame.registers[0]==frame.registers[0]);
    // Other user MSR changes are not permission to change trusted privilege state.
    memory=clean;memory.number(mc+4*o::pt_msr,0xffffffffu&~C::unsupported_msr,4);
    CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));CHECK(result.frame.flags==handler.flags);
    for(unsigned bit=0;bit<32;++bit)
    {
        memory=clean;memory.number(mc+C::spefscr_offset,u64{1}<<bit,4);
        if(!(o::spefscr_defined_bits&(u32{1}<<bit)))reject();
        else { CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));CHECK(C::backend_get(result.frame,264,4)==(u64{1}<<bit)); }
        if(C::unsupported_msr&(u32{1}<<bit)) { memory=clean;memory.number(mc+4*o::pt_msr,handler.flags|(u32{1}<<bit),4);reject(); }
    }
    for(usize at=C::fp_offset;at<C::pad_offset;++at) {memory=clean;memory.number(mc+at,1,1);reject();}
    for(usize at=C::spefscr_offset+4;at<C::context_bytes;++at) {memory=clean;memory.number(mc+at,1,1);reject();}
    for(u64 bad:{u64{0},begin-4,begin+1,end,end+4}) {memory=clean;memory.number(mc+4*o::pt_nip,bad,4);reject();}
    for(u64 bad:{u64{0},begin-1,begin,end+16}) {memory=clean;memory.number(mc+4,bad,4);reject();}
    // Interrupted r1 is a restored register, not the signal frame address.
    // Exercise all alignment residues, including the pinned musl EABI r1 % 16 == 8.
    for(u64 offset=0;offset<16;++offset)
    {
        memory=clean;memory.number(mc+4,frame.sp+offset,4);
        CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));
        CHECK(result.frame.sp==frame.sp+offset && result.frame.registers[1]==frame.sp+offset);
        CHECK(C::backend_get(result.frame,12,4)==frame.sp+offset);
        CHECK(result.frame.pc==frame.pc && result.frame.flags==handler.flags);
        // A created signal frame remains 16-byte aligned even after such a return.
        typename C::Placement next;
        CHECK(C::place(result.frame.sp,begin,end,next));
        CHECK(!(next.stack_pointer&15) && C::placement(next,begin,end));
    }
    memory=clean;memory.number(mc+4*o::pt_mq,1,4);reject();
    memory=clean;memory.number(mc+44*4,1,4);reject();
    if constexpr(C::realtime) {memory=clean;memory.number(p.frame_address+128+4,0x1234,4);reject();}
    const u64 pointer=p.frame_address+(C::realtime?128+o::uc_regs:o::sc_regs);
    for(u64 bad:{u64{0},begin-16,begin+1,end-16,end}) {memory=clean;memory.number(pointer,bad,4);reject();}
    memory=clean;
    const u64 relocated=memory_base+0x100;
    for(usize i=0;i<C::context_bytes;++i)memory.bytes[relocated-memory_base+i]=memory.bytes[mc-memory_base+i];
    memory.number(pointer,relocated,4);memory.number(mc+4*o::pt_nip,0,4);memory.number(relocated+12,0x12345678,4);
    CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));CHECK(result.frame.registers[3]==0x12345678);
    // Hostile placement never publishes tentative coordinates.
    for(u64 bad:{u64{0},begin,begin+C::allocation_bytes-1,end+1,~u64{0}})
    {
        typename C::Placement unchanged{71,93};CHECK(!C::place(bad,begin,end,unchanged));CHECK(unchanged==(typename C::Placement{71,93}));
    }
    for(unsigned n=0;n<16;++n) {typename C::Placement aligned;CHECK(C::place(top+n,begin,end,aligned));CHECK(aligned==p);}
    auto invalid_capture=capture;invalid_capture.metadata_supplied=false;auto old_image=image;
    CHECK(!C::encode(frame,invalid_capture,mask,stack,info,p,begin+0x3000,begin,end,image));CHECK(image.bytes==old_image.bytes);
    invalid_capture=capture;invalid_capture.contract={};CHECK(!C::encode(frame,invalid_capture,mask,stack,info,p,begin+0x3000,begin,end,image));
    auto invalid_contract=contract;invalid_contract.resume=C::Resume::unverified;
    CHECK(!C::supported(frame,invalid_contract));invalid_contract=contract;invalid_contract.hardware=C::Hardware::unverified;CHECK(!C::supported(frame,invalid_contract));
    auto invalid_frame=frame;invalid_frame.flags|=1;CHECK(!C::supported(invalid_frame,contract));
    invalid_frame=frame;invalid_frame.flags&=~C::msr_spe;CHECK(!C::supported(invalid_frame,contract));
    if constexpr(sizeof(uptr)>4) {invalid_frame=frame;invalid_frame.registers[31]=u64{1}<<32;CHECK(!C::supported(invalid_frame,contract));}
    invalid_frame=frame;invalid_frame.sp=0;
    CHECK(C::encode(invalid_frame,capture,mask,stack,info,p,begin+0x3000,begin,end,image));
    CHECK(C::get(image.bytes,0,4)==0 && C::get(image.bytes,C::context_offset+4,4)==0);
    CHECK(C::setup_handler(invalid_frame,contract,p,info,begin+0x3000,begin+0x4000,begin,end,handler));
    memory=clean;CHECK(memory.write(p.stack_pointer,std::span<const std::byte>{image.bytes}));
    memory.number(mc+4,memory_base+0x5100,4);
    CHECK(C::decode(p.stack_pointer,handler,contract,begin,end,read,result));
    CHECK(result.frame.sp==memory_base+0x5100 && result.frame.registers[1]==result.frame.sp);
    for(u64 bad:{u64{0},begin+1,end-4,end})
    {
        auto untouched=frame;CHECK(!C::setup_handler(frame,contract,p,info,begin+0x3000,bad,begin,end,untouched));CHECK(equal(untouched,frame));
    }
    auto untouched=frame;typename C::Placement invalid_p{p.frame_address+16,p.stack_pointer};
    CHECK(!C::setup_handler(frame,contract,invalid_p,info,begin+0x3000,begin+0x4000,begin,end,untouched));CHECK(equal(untouched,frame));
}
int main()
{
    exercise<Ppc32LegacySignalFrame>();exercise<Ppc32RtSignalFrame>();
    std::printf("PPC32_SIGNAL_FRAME_MODEL_PASS checks=%zu legacy=1 rt=1 scalar_spe_only=1 native_execution=0\n",checks);
}
