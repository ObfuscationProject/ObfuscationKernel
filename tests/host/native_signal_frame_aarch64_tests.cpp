#include "ok/user/native_signal_frame_aarch64.hpp"
#include "../fixtures/signals/linux_aarch64_layout.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
using namespace ok;
using C=ok::user::native::Aarch64SignalFrame;
using SignalMask=ok::sched::SignalMask;
namespace L=linux_aarch64_layout;
static usize checks{};
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"line=%d expression=%s\n",__LINE__,#x);std::abort();}}while(0)
constexpr u64 begin=0x10000000,end=0x40000000,address=0x20000000;
using Bytes=std::array<std::byte,L::frame_bytes+L::record_bytes>;
static u64 random_state=0x391a662cd483758fULL;
u64 next(){random_state^=random_state<<13;random_state^=random_state>>7;random_state^=random_state<<17;return random_state;}
void store(Bytes &b,usize at,u64 value,usize size){for(usize i=0;i<size;++i){b[at+i]=std::byte(value&255);value/=256;}}
bool same(const C::Frame &a,const C::Frame &b){return a.registers==b.registers&&a.pc==b.pc&&a.sp==b.sp&&a.flags==b.flags&&a.fs_base==b.fs_base&&a.gs_base==b.gs_base&&a.fp_state==b.fp_state;}
C::Frame frame()
{
    C::Frame f;for(auto &r:f.registers)r=next();for(auto &v:f.fp_state)v=static_cast<u8>(next());
    f.pc=begin+0x1000;f.sp=end-0x10000;f.flags=next()&0xf0000000;f.fs_base=begin+0x6000;f.gs_base=begin+0x7000;return f;
}
Bytes linux_fixture(const C::Frame &f,SignalMask mask,C::Info info)
{
    Bytes bytes{};
    store(bytes,L::info_signal,static_cast<u32>(info.signal),4);store(bytes,L::info_code,static_cast<u32>(info.code),4);
    store(bytes,L::info_pid,info.sender_pid,4);store(bytes,L::info_uid,info.sender_uid,4);
    store(bytes,L::stack_offset+L::stack_flags,2,4);
    store(bytes,L::mask_offset,mask.words[0]&~((1ULL<<8)|(1ULL<<18)),8);
    for(usize i=0;i<31;++i)store(bytes,L::registers_offset+8*i,f.registers[i],8);
    store(bytes,L::sp_offset,f.sp,8);store(bytes,L::pc_offset,f.pc,8);store(bytes,L::pstate_offset,f.flags,8);
    store(bytes,L::reserved_offset,0x46508001,4);store(bytes,L::reserved_offset+4,L::fpsimd_bytes,4);
    store(bytes,L::reserved_offset+L::fpsr_offset,f.registers[31]>>32,4);
    store(bytes,L::reserved_offset+L::fpcr_offset,f.registers[31]&0xffffffff,4);
    for(usize i=0;i<512;++i)bytes[L::reserved_offset+L::vectors_offset+i]=std::byte(f.fp_state[i]);
    store(bytes,L::frame_bytes,f.registers[29],8);store(bytes,L::frame_bytes+L::record_lr,f.registers[30],8);
    return bytes;
}
bool restore(const Bytes &bytes,const C::Frame &current,C::Restored &out,usize prefix=L::frame_bytes,u64 at=address)
{
    unsigned copies{};
    auto read=[&](u64 from,std::span<std::byte> data){++copies;CHECK(from==address&&data.size()==L::frame_bytes);std::copy_n(bytes.begin(),prefix,data.begin());return prefix==L::frame_bytes;};
    bool result=C::decode(at,current,begin,end,read,out);CHECK(copies==(at==address?1U:0U));return result;
}
void reject(const Bytes &bytes,const C::Frame &current)
{
    C::Restored out;out.frame=frame();out.mask={{0x1234,0x5678}};out.stack={1,2,3};const auto before=out;
    CHECK(!restore(bytes,current,out));CHECK(same(out.frame,before.frame)&&out.mask==before.mask&&out.stack==before.stack);
}
void layout_and_values()
{
    CHECK(C::frame_bytes==L::frame_bytes&&C::image_bytes==L::frame_bytes+L::record_bytes);
    CHECK(C::ucontext_offset==L::ucontext_offset&&C::context_offset==L::context_offset&&C::stack_offset==L::stack_offset);
    CHECK(C::mask_offset==L::mask_offset&&C::registers_offset==L::registers_offset&&C::sp_offset==L::sp_offset);
    CHECK(C::pc_offset==L::pc_offset&&C::pstate_offset==L::pstate_offset&&C::reserved_offset==L::reserved_offset);
    CHECK(C::reserved_bytes==L::reserved_bytes&&C::fpsimd_bytes==L::fpsimd_bytes&&L::info_bytes==128&&L::vector_bytes==512);
    for(unsigned round=0;round<600;++round)
    {
        auto saved=frame(),current=frame();SignalMask mask{{next(),~u64{0}}};C::Info info{static_cast<i32>(1+round%64),round%2?-6:0,static_cast<u32>(next()),static_cast<u32>(next())};
        const auto expected=linux_fixture(saved,mask,info);C::Image encoded;
        CHECK(C::encode(saved,mask,{},info,begin,end,encoded));CHECK(encoded.bytes==expected);
        // Decode a separately constructed Linux-layout fixture, not encode output.
        C::Restored result;CHECK(restore(expected,current,result));saved.fs_base=current.fs_base;saved.gs_base=current.gs_base;
        CHECK(same(saved,result.frame));CHECK(result.mask.words[0]==(mask.words[0]&~((1ULL<<8)|(1ULL<<18)))&&result.mask.words[1]==0);
        CHECK(result.stack==C::Stack{});
    }
}
void records_and_faults()
{
    const auto f=frame(),current=frame();const auto raw=linux_fixture(f,{}, {10,-6,23,42});
    for(u64 size:{0ULL,7ULL,8ULL,15ULL,16ULL,512ULL,527ULL,529ULL,544ULL,4096ULL,0xffffffffULL})
    {auto b=raw;store(b,L::reserved_offset+4,size,4);reject(b,current);}
    for(u64 magic:{0ULL,0x53564501ULL,0x45585401ULL,0x54366345ULL,0x5a544e01ULL,0x54504902ULL,0x46504d52ULL,0x504f4530ULL,0x47435300ULL,0xdeadbeefULL})
    {auto b=raw;store(b,L::reserved_offset,magic,4);reject(b,current);}
    {auto b=raw;store(b,L::reserved_offset,0,4);store(b,L::reserved_offset+4,0,4);reject(b,current);}
    {auto b=raw;store(b,L::reserved_offset+L::fpsimd_bytes+4,16,4);reject(b,current);}
    {auto b=raw;std::copy_n(raw.begin()+L::reserved_offset,L::fpsimd_bytes,b.begin()+L::reserved_offset+L::fpsimd_bytes);reject(b,current);}
    // Every legal aligned placement after informational ESR records is accepted.
    for(usize pos=L::reserved_offset;pos+L::fpsimd_bytes+16<=L::frame_bytes;pos+=16)
    {
        auto b=raw;std::fill(b.begin()+L::reserved_offset,b.begin()+L::frame_bytes,std::byte{});
        for(usize at=L::reserved_offset;at<pos;at+=16){store(b,at,0x45535201,4);store(b,at+4,16,4);store(b,at+8,~u64{0},8);}
        std::copy_n(raw.begin()+L::reserved_offset,L::fpsimd_bytes,b.begin()+pos);
        std::fill(b.begin()+pos+L::fpsimd_bytes+16,b.begin()+L::frame_bytes,std::byte{0xa5});
        C::Restored out;CHECK(restore(b,current,out));CHECK(out.frame.registers==f.registers&&out.frame.fp_state==f.fp_state);
    }
    // Fill to the exact reserved boundary without a terminator.
    {auto b=raw;for(usize at=L::reserved_offset+L::fpsimd_bytes;at<L::frame_bytes;at+=16){store(b,at,0x45535201,4);store(b,at+4,16,4);}reject(b,current);}
    for(u64 size:{0ULL,8ULL,15ULL,32ULL,0xfffffff0ULL})
    {auto b=raw;store(b,L::reserved_offset+L::fpsimd_bytes,0x45535201,4);store(b,L::reserved_offset+L::fpsimd_bytes+4,size,4);reject(b,current);}
    for(usize prefix=0;prefix<L::frame_bytes;++prefix)
    {
        C::Restored out;out.frame=current;out.mask={{7,9}};out.stack={1,2,3};const auto before=out;
        CHECK(!restore(raw,current,out,prefix));CHECK(same(out.frame,before.frame)&&out.mask==before.mask&&out.stack==before.stack);
    }
}
void user_validation()
{
    const auto f=frame();const auto raw=linux_fixture(f,{}, {10,0,1,2});
    for(unsigned bit=0;bit<64;++bit)
    {
        auto b=raw;store(b,L::pstate_offset,u64{1}<<bit,8);
        if(bit>=28&&bit<=31){C::Restored out;CHECK(restore(b,f,out));CHECK(out.frame.flags==(u64{1}<<bit));}else reject(b,f);
    }
    for(u64 pc:std::array<u64,7>{0,begin-4,begin+1,end,end+4,0xffff000000001000ULL,~u64{0}}){auto b=raw;store(b,L::pc_offset,pc,8);reject(b,f);}
    for(u64 sp:std::array<u64,6>{0,begin,begin+1,end+16,0xffff000000001000ULL,~u64{0}}){auto b=raw;store(b,L::sp_offset,sp,8);reject(b,f);}
    for(u64 flags:{0ULL,1ULL,3ULL,0x80000000ULL,0xffffffffULL}){auto b=raw;store(b,L::stack_offset+L::stack_flags,flags,4);reject(b,f);}
    {auto b=raw;store(b,L::ucontext_offset,~u64{0},8);store(b,L::ucontext_offset+8,~u64{0},8);store(b,L::context_offset,~u64{0},8);
     store(b,L::stack_offset,~u64{0},8);store(b,L::stack_offset+L::stack_size,~u64{0},8);
     C::Restored out;CHECK(restore(b,f,out));CHECK(out.stack==C::Stack{});CHECK(same(out.frame,f));}
    C::Restored out;CHECK(!restore(raw,f,out,L::frame_bytes,address+1));
    unsigned reads{};CHECK(!C::decode(end-16,f,begin,end,[&](u64,auto){++reads;return true;},out)&&reads==0);
    CHECK(!C::decode(address,f,begin,~u64{0},[&](u64,auto){++reads;return true;},out)&&reads==0);
}
void placement_and_handler()
{
    for(u64 top=end-31;top<=end;++top)
    {
        C::Placement p;CHECK(C::place(top,begin,end,p));CHECK(p.record_address==((top-16)&~u64{15}));
        CHECK(p.address==p.record_address-L::frame_bytes&&!(p.address&15));
    }
    C::Placement p{3,4};for(u64 top:std::array<u64,6>{0,begin,begin+16,begin+C::image_bytes-1,end+1,~u64{0}}){auto before=p;CHECK(!C::place(top,begin,end,p));CHECK(p==before);}
    CHECK(C::place(begin+C::image_bytes,begin,end,p)&&p.address==begin);
    CHECK(!C::place(end,begin,begin,p));CHECK(!C::place(end,begin,~u64{0},p));
    CHECK(C::place(end,begin,end,p));const auto f=frame();C::Frame h;
    CHECK(C::setup_handler(f,p,{10,0,2,3},begin+4,begin+8,true,begin,end,h));
    CHECK(h.pc==begin+4&&h.sp==p.address&&h.registers[0]==10&&h.registers[1]==p.address&&h.registers[2]==p.address+128);
    CHECK(h.registers[29]==p.record_address&&h.registers[30]==begin+8&&h.registers[31]==f.registers[31]);
    CHECK(h.fs_base==f.fs_base&&h.gs_base==f.gs_base&&h.fp_state==f.fp_state);
    CHECK(C::setup_handler(f,p,{10,0,2,3},begin+4,begin+8,false,begin,end,h));CHECK(h.registers[1]==f.registers[1]&&h.registers[2]==f.registers[2]);
    const auto old=h;CHECK(!C::setup_handler(f,p,{10,0,2,3},begin+1,begin+8,true,begin,end,h)&&same(h,old));
    CHECK(!C::setup_handler(f,{p.address,p.record_address+16},{10,0,2,3},begin+4,begin+8,true,begin,end,h)&&same(h,old));
    CHECK(!C::setup_handler(f,p,{10,0,2,3},begin+4,begin+1,true,begin,end,h)&&same(h,old));
    CHECK(!C::setup_handler(f,p,{10,1,2,3},begin+4,begin+8,true,begin,end,h)&&same(h,old));
    C::Image image;image.bytes.fill(std::byte{0x55});const auto original=image.bytes;auto bad=f;bad.flags=1;
    CHECK(!C::encode(bad,{}, {},{10,0,1,2},begin,end,image)&&image.bytes==original);
    CHECK(!C::encode(f,{}, {0,0,0},{10,0,1,2},begin,end,image)&&image.bytes==original);
    CHECK(!C::encode(f,{}, {},{10,1,1,2},begin,end,image)&&image.bytes==original);
    CHECK(!C::encode(f,{}, {},{65,0,1,2},begin,end,image)&&image.bytes==original);
}
struct FixtureStackPolicy
{
    bool operator()(C::Stack value,C::Stack &out)const noexcept
    {
        if(value!=C::Stack{begin+0x1000,0x8000,0})return false;
        out=value;return true;
    }
};
struct ThrowingStackPolicy {bool operator()(C::Stack,C::Stack&)const{return true;}};
template<typename Policy> concept PolicyAccepted=requires(const C::Frame &f,C::Image &out,Policy p){C::encode(f,{}, {},{},begin,end,out,p);};
static_assert(PolicyAccepted<FixtureStackPolicy>);
static_assert(!PolicyAccepted<ThrowingStackPolicy>);
void stack_policy_boundary()
{
    const auto f=frame();const C::Stack configured{begin+0x1000,0x8000,0};C::Image image;
    CHECK(C::encode(f,{},configured,{10,0,1,2},begin,end,image,FixtureStackPolicy{}));
    auto expected=linux_fixture(f,{}, {10,0,1,2});store(expected,L::stack_offset,configured.pointer,8);
    store(expected,L::stack_offset+L::stack_flags,configured.flags,4);store(expected,L::stack_offset+L::stack_size,configured.size,8);
    CHECK(image.bytes==expected);reject(expected,f);
    C::Restored out;
    CHECK(C::decode(address,f,begin,end,[&](u64,std::span<std::byte> bytes){std::copy_n(expected.begin(),bytes.size(),bytes.begin());return true;},out,FixtureStackPolicy{}));
    CHECK(out.stack==configured&&same(out.frame,f));
}
int main(){layout_and_values();records_and_faults();user_validation();placement_and_handler();stack_policy_boundary();std::printf("PASS AArch64 FPSIMD frame checks=%zu hosted=1 delivery=0\n",checks);}
