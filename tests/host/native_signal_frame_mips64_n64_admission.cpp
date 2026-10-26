#include "ok/arch/mips64/signal.hpp"
#include <cstdio>
#include <cstdlib>
using namespace ok;
using C=ok::user::native::Mips64N64SignalFrame;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"ADMISSION FAIL %d\n",__LINE__);std::abort();}}while(0)
static bool same(const C::Frame&a,const C::Frame&b){return a.registers==b.registers&&a.pc==b.pc&&a.sp==b.sp&&a.flags==b.flags&&a.fs_base==b.fs_base&&a.gs_base==b.gs_base&&a.fp_state==b.fp_state;}
int main()
{
 constexpr u64 lo=0x40000000,hi=0x80000000,at=0x70000000;
 C::Frame f;f.pc=lo;f.sp=at;f.flags=0x240080b3;f.fs_base=0x71234560;f.registers[26]=0x1122334455667788;f.registers[27]=0x8877665544332211;
 C::Capture valid{C::FpContract::eager_fr1_legacy_nan_no_extensions,C::ResumeContract::normalized_resume_pc};
 C::Info info{16,0,0xffffffff,0xffffffff};C::Stack stack{0,0,2};
 auto attempt=[&](C::Frame current,C::Capture capture,C::Info i,u64 top,u64 handler,u64 restorer,u64 begin,u64 end){C::Placement placement{0xface};auto result=f;result.pc=0xbeef;auto before=result;unsigned calls=0;auto write=[&](u64,std::span<const std::byte>){++calls;return true;};CHECK(!C::prepare(current,capture,{},stack,i,top,true,handler,restorer,begin,end,write,placement,result));CHECK(calls==0);CHECK(placement.address==0xface);CHECK(same(result,before));};
 for(u64 pc:{u64(0),lo-4,lo+1,hi,~u64(0)}){auto current=f;current.pc=pc;attempt(current,valid,info,at,lo,lo+8,lo,hi);}
 for(u64 handler:{u64(0),lo-4,lo+1,hi,~u64(0)})attempt(f,valid,info,at,handler,lo+8,lo,hi);
 for(u64 restorer:{u64(0),lo-4,lo+1,hi-4,hi,~u64(0)})attempt(f,valid,info,at,lo,restorer,lo,hi);
 for(u64 top:{u64(0),lo,lo+807,hi+1,~u64(0)})attempt(f,valid,info,top,lo,lo+8,lo,hi);
 for(auto bounds:{std::pair<u64,u64>{hi,hi},{hi,lo},{lo,hi+1},{0,~u64(0)}})attempt(f,valid,info,at,lo,lo+8,bounds.first,bounds.second);
 for(C::Capture c:{C::Capture{},C::Capture{C::FpContract::unverified,C::ResumeContract::normalized_resume_pc},C::Capture{C::FpContract::eager_fr1_legacy_nan_no_extensions,C::ResumeContract::unverified},C::Capture{static_cast<C::FpContract>(99),C::ResumeContract::normalized_resume_pc}}){attempt(f,c,info,at,lo,lo+8,lo,hi);auto out=f;CHECK(!C::setup_handler(f,c,{at},info,lo,lo+8,lo,hi,out));CHECK(same(out,f));}
 for(u64 status:{u64(0),u64(0x20008013),u64(0x04008013)}){auto current=f;current.flags=status;attempt(current,valid,info,at,lo,lo+8,lo,hi);auto out=f;CHECK(!C::setup_handler(current,valid,{at},info,lo,lo+8,lo,hi,out));CHECK(same(out,f));}
 for(u32 csr:{0x80u|0x1000u,0x20000u,0x40000u,0x80000u,0x100000u,0x200000u,0x400000u}){auto current=f;for(unsigned n=0;n<4;++n)current.fp_state[256+n]=u8(csr>>(24-n*8));attempt(current,valid,info,at,lo,lo+8,lo,hi);auto out=f;CHECK(!C::setup_handler(current,valid,{at},info,lo,lo+8,lo,hi,out));CHECK(same(out,f));}
 for(i32 code:{-1,1,128}){auto bad=info;bad.code=code;attempt(f,valid,bad,at,lo,lo+8,lo,hi);}
 for(i32 signo:{0,-1,129}){auto bad=info;bad.signal=signo;attempt(f,valid,bad,at,lo,lo+8,lo,hi);}
 for(i32 signo:{1,64,65,128})for(i32 code:{0,-6}){C::Info i{signo,code,0xffffffff,0xffffffff};C::Placement p;C::Frame out;unsigned calls=0;auto write=[&](u64 ptr,std::span<const std::byte> bytes){++calls;CHECK(ptr==((at-808)&~u64(15)));CHECK(bytes.size()==808);CHECK(C::get(std::array<std::byte,4>{bytes[24],bytes[25],bytes[26],bytes[27]},0,4)==u32(signo));return true;};CHECK(C::prepare(f,valid,{},stack,i,at,true,hi-4,hi-8,lo,hi,write,p,out));CHECK(calls==1);CHECK(out.pc==hi-4&&out.registers[4]==u32(signo));CHECK(out.registers[26]==f.registers[26]&&out.registers[27]==f.registers[27]&&out.fs_base==f.fs_base);}
 std::printf("PASS MIPS64 n64 admission checks=%u\n",checks);
}
