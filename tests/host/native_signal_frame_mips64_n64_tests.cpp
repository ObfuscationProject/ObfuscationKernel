#include "ok/user/native_signal_frame_mips64_n64.hpp"
#include "../fixtures/signals/linux_mips64_n64_layout.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace ok;
using namespace ok::user::native;
using C=Mips64N64SignalFrame;
namespace L=linux_n64_oracle;
static unsigned long checks;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#x);std::abort();}}while(0)
template<class B>static void write(B &b,usize off,u64 v,usize n){for(usize i=0;i<n;++i)b[off+i]=std::byte(v>>(8*(n-1-i)));}
template<class B>static u64 read(const B &b,usize off,usize n){u64 v=0;for(usize i=0;i<n;++i)v=v*256+std::to_integer<unsigned>(b[off+i]);return v;}
static void state(C::Frame &f,usize off,u64 v,usize n){for(usize i=0;i<n;++i)f.fp_state[off+i]=u8(v>>(8*(n-1-i)));}
static u64 state(const C::Frame &f,usize off,usize n){u64 v=0;for(usize i=0;i<n;++i)v=v*256+f.fp_state[off+i];return v;}
static bool equal(const C::Frame&a,const C::Frame&b){return a.registers==b.registers&&a.pc==b.pc&&a.sp==b.sp&&a.flags==b.flags&&a.fs_base==b.fs_base&&a.gs_base==b.gs_base&&a.fp_state==b.fp_state;}
static bool equal(const C::ReturnPlan&a,const C::ReturnPlan&b){return equal(a.frame,b.frame)&&a.mask==b.mask&&a.stack==b.stack&&a.restore_altstack==b.restore_altstack&&a.fcsr_patch==b.fcsr_patch&&a.pending==b.pending;}
int main()
{
 constexpr u64 begin=0x40000000,end=0x80000000,frame_at=0x70000000;
 constexpr usize context=L::uc+L::uc_context,mask_at=L::uc+L::uc_mask;
 CHECK(C::frame_bytes==L::frame_bytes);CHECK(C::context_offset==context);CHECK(C::mask_offset==mask_at);
 CHECK(C::pc_offset==context+L::pc);CHECK(C::fp_offset==context+L::fpregs);CHECK(C::fcsr_offset==context+L::fcsr);CHECK(C::hi_offset==context+L::hi);CHECK(C::lo_offset==context+L::lo);
 CHECK(C::return_syscall==5211);CHECK(C::restorer_words()[0]==0x2402145b);CHECK(C::restorer_words()[1]==12);
 const C::Capture cap{C::FpContract::eager_fr1_legacy_nan_no_extensions,C::ResumeContract::normalized_resume_pc};
 C::Frame source;source.pc=begin+0x2000;source.sp=0x72000000;source.flags=0x240080b3;source.fs_base=0x7123456789abcdef;source.gs_base=0xfedcba9876543210;
 for(unsigned i=0;i<32;++i)source.registers[i]=0x8765432100000000ULL+u64(i)*0x123456789;
 for(unsigned i=0;i<512;++i)source.fp_state[i]=u8(i*31+97);
 state(source,256,0xa1800073,4);state(source,272,0x812345679abcdef0,8);state(source,280,0xfedcba9812345670,8);
 const C::Stack stack{0xffff800012345678,0xfedcba9876543210,0xabcdef01};const C::Info info{127,-6,0x80000001,0xfedcba98};SignalMask mask;mask.words={~u64(0),0x87654321fedcba98};
 C::Image image;CHECK(C::encode(source,cap,mask,stack,info,begin,end,image));C::Image expected;
 write(expected.bytes,L::info,127,4);write(expected.bytes,L::info+L::info_code,0xfffffffa,4);write(expected.bytes,L::info+L::info_pid,info.sender_pid,4);write(expected.bytes,L::info+L::info_uid,info.sender_uid,4);
 write(expected.bytes,L::uc+L::uc_stack,stack.pointer,8);write(expected.bytes,L::uc+L::uc_stack+L::stack_size,stack.size,8);write(expected.bytes,L::uc+L::uc_stack+L::stack_flags,stack.flags,4);
 for(unsigned i=1;i<32;++i)if(i!=26&&i!=27)write(expected.bytes,context+L::regs+8*i,i==29?source.sp:source.registers[i],8);
 for(unsigned i=0;i<256;++i)expected.bytes[context+L::fpregs+i]=std::byte(source.fp_state[i]);
 write(expected.bytes,context+L::hi,state(source,272,8),8);write(expected.bytes,context+L::lo,state(source,280,8),8);write(expected.bytes,context+L::pc,source.pc,8);write(expected.bytes,context+L::fcsr,state(source,256,4),4);write(expected.bytes,context+L::used,L::USED_FP|L::USED_FR1,4);
 auto sanitized=mask;sanitized.set(9,false);sanitized.set(23,false);write(expected.bytes,mask_at,sanitized.words[0],8);write(expected.bytes,mask_at+8,sanitized.words[1],8);
 for(usize i=0;i<image.bytes.size();++i) { CHECK(image.bytes[i]==expected.bytes[i]); }
 const auto good=image;
 auto input=[&](u64 addr,std::span<std::byte> dest){CHECK(addr==frame_at);CHECK(dest.size()==808);std::memcpy(dest.data(),image.bytes.data(),dest.size());return true;};
 C::ReturnPlan returned;CHECK(C::decode(frame_at,source,cap,begin,end,input,returned));auto want=source;want.registers[0]=0;want.registers[29]=source.sp;CHECK(equal(returned.frame,want));CHECK(returned.mask==sanitized);CHECK(returned.stack==stack);CHECK(returned.restore_altstack);CHECK(!returned.fcsr_patch.required);CHECK(returned.pending==C::Pending::none);
 for(unsigned reg=0;reg<32;++reg){image=good;u64 value=reg==29?0x71234008:0xf123456789abcdef;write(image.bytes,context+reg*8,value,8);CHECK(C::decode(frame_at,source,cap,begin,end,input,returned));auto edited=want;if(reg&&reg!=26&&reg!=27)edited.registers[reg]=value;if(reg==29)edited.sp=value;CHECK(equal(returned.frame,edited));}
 for(unsigned slot=0;slot<32;++slot){image=good;write(image.bytes,context+L::fpregs+8*slot,0xabcdef0123456789ULL+slot,8);CHECK(C::decode(frame_at,source,cap,begin,end,input,returned));auto edited=want;state(edited,8*slot,0xabcdef0123456789ULL+slot,8);CHECK(equal(returned.frame,edited));}
 for(auto pair:{std::pair<usize,usize>{context+L::hi,272},{context+L::lo,280}}){image=good;write(image.bytes,pair.first,0xffffffff12345678,8);CHECK(C::decode(frame_at,source,cap,begin,end,input,returned));CHECK(state(returned.frame,pair.second,8)==0xffffffff12345678);}
 for(u32 enable=0;enable<32;++enable)for(u32 cause=0;cause<64;++cause){image=good;u32 csr=0xa1800073|(enable<<7)|(cause<<12);write(image.bytes,context+L::fcsr,csr,4);CHECK(C::decode(frame_at,source,cap,begin,end,input,returned));u32 bits=(enable<<12)|0x20000;bool pending=csr&bits;u32 normalized=pending?csr&~bits:csr;CHECK(state(returned.frame,256,4)==normalized);CHECK(returned.fcsr_patch.required==pending);CHECK((returned.pending==C::Pending::sigfpe)==pending);if(pending){CHECK(returned.fcsr_patch.address==frame_at+context+L::fcsr);CHECK(read(returned.fcsr_patch.bytes,0,4)==normalized);}else CHECK(returned.fcsr_patch==C::FcsrPatch{});auto f=source;state(f,256,csr,4);auto encoded=good;CHECK(C::encode(f,cap,mask,stack,info,begin,end,encoded)==!pending);if(pending)CHECK(encoded.bytes==good.bytes);}
 C::ReturnPlan poison=returned;poison.frame.pc=0xfeed;poison.mask.words={1,2};poison.stack={3,4,5};poison.fcsr_patch={6,{std::byte{7},std::byte{8},std::byte{9},std::byte{10}},true};
 auto reject=[&](){auto out=poison;CHECK(!C::decode(frame_at,source,cap,begin,end,input,out));CHECK(equal(out,poison));};
 for(unsigned bit=0;bit<32;++bit){image=good;write(image.bytes,context+L::used,3u^(u32(1)<<bit),4);reject();}
 for(usize at:{context+L::hi1,context+L::hi1+8,context+L::hi1+16,context+L::lo+8,context+L::lo+16,context+L::lo3}){image=good;write(image.bytes,at,1,8);reject();}
 image=good;write(image.bytes,context+L::dsp,0xffffffff,4);reject();for(unsigned bit=18;bit<23;++bit){image=good;write(image.bytes,context+L::fcsr,u32(1)<<bit,4);reject();}
 for(u64 pc:{u64(0),begin-4,begin+1,end,end+4,u64(0xffffffff40002000)}){image=good;write(image.bytes,context+L::pc,pc,8);reject();}
 for(u64 sp:{u64(0),begin,begin+1,end+8,u64(0xffffffff72000000)}){image=good;write(image.bytes,context+29*8,sp,8);reject();}
 // Raw stack/unused metadata edits are not trusted privilege or mapping requests.
 image=good;write(image.bytes,context+L::reserved,0xffffffff,4);write(image.bytes,L::uc,~u64(0),8);write(image.bytes,L::uc+8,~u64(0),8);write(image.bytes,L::uc+L::uc_stack,~u64(0),8);write(image.bytes,L::uc+L::uc_stack+8,~u64(0),8);CHECK(C::decode(frame_at,source,cap,begin,end,input,returned));CHECK(equal(returned.frame,want));CHECK(returned.stack.pointer==~u64(0)&&returned.stack.size==~u64(0));
 for(u64 sp:{u64(0),u64(1),~u64(0)}){auto f=source;f.sp=sp;C::Image out;CHECK(C::encode(f,cap,mask,stack,info,begin,end,out));CHECK(read(out.bytes,context+29*8,8)==sp);}
 auto refuse=[&](const C::Frame& f,C::Capture c,C::Info i){auto out=good;CHECK(!C::encode(f,c,mask,stack,i,begin,end,out));CHECK(out.bytes==good.bytes);};
 for(u64 status:{u64(0),u64(0x20008013),u64(0x04008013)}){auto f=source;f.flags=status;refuse(f,cap,info);}auto c=cap;c.fp=C::FpContract::unverified;refuse(source,c,info);c=cap;c.resume=C::ResumeContract::unverified;refuse(source,c,info);
 for(i32 code:{-1,1,128}){auto bad=info;bad.code=code;refuse(source,cap,bad);}for(i32 signal:{-1,0,129}){auto bad=info;bad.signal=signal;refuse(source,cap,bad);}
 image=good;
 for(usize cut=0;cut<=808;++cut){auto out=poison;auto partial=[&](u64,std::span<std::byte> target){std::memcpy(target.data(),image.bytes.data(),cut);return false;};CHECK(!C::decode(frame_at,source,cap,begin,end,partial,out));CHECK(equal(out,poison));C::Placement p{99};auto f=source;auto before=f;unsigned calls=0;std::array<std::byte,808> user;user.fill(std::byte{0xdd});auto output=[&](u64 at,std::span<const std::byte>b){++calls;CHECK((at&15)==0);std::memcpy(user.data(),b.data(),cut);return false;};CHECK(!C::prepare(source,cap,mask,stack,info,frame_at,false,begin,begin+8,begin,end,output,p,f));CHECK(calls==1&&p.address==99);CHECK(equal(f,before));for(usize i=cut;i<808;++i)CHECK(user[i]==std::byte{0xdd});}
 for(u64 low:{u64(0),begin,begin+7,begin+15,end-2048})for(u64 n=0;n<2048;++n)for(bool alt:{false,true}){u64 top=low+n,bytes=808+(alt?0:32),address=0;bool ok=n>=bytes&&top<=end&&top>low;if(ok){address=(top-bytes)/16*16;ok=address>=low&&address+808<=end;}C::Placement p{123};CHECK(C::place(top,alt,low,end,p)==ok);CHECK(p.address==(ok?address:123));}
 C::Frame handler;CHECK(C::setup_handler(source,cap,{frame_at},info,begin,end-8,begin,end,handler));auto hwant=source;hwant.pc=hwant.registers[25]=begin;hwant.sp=hwant.registers[29]=frame_at;hwant.registers[0]=0;hwant.registers[4]=127;hwant.registers[5]=frame_at+L::info;hwant.registers[6]=frame_at+L::uc;hwant.registers[31]=end-8;CHECK(equal(handler,hwant));
 for(u64 restorer:{begin+1,end-4,end,~u64(0)}){auto out=source;CHECK(!C::setup_handler(source,cap,{frame_at},info,begin,restorer,begin,end,out));CHECK(equal(out,source));}
 for(u64 ptr:{frame_at+8,end-800,~u64(0)}){unsigned calls=0;auto in=[&](u64,std::span<std::byte>){++calls;return true;};auto out=poison;CHECK(!C::decode(ptr,source,cap,begin,end,in,out));CHECK(calls==0);CHECK(equal(out,poison));}
 {unsigned calls=0;auto in=[&](u64,std::span<std::byte>){++calls;return true;};auto out=poison;CHECK(!C::decode(frame_at,source,{},begin,end,in,out));CHECK(calls==0);CHECK(equal(out,poison));}
 std::printf("PASS MIPS64 n64 author checks=%lu\n",checks);
}
