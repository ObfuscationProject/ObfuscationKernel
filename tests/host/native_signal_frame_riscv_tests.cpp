#include "ok/user/native_signal_frame_riscv.hpp"
#include "../fixtures/signals/linux_riscv_layout.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace ok;
using namespace ok::user::native;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
template <usize N> void wire(std::array<std::byte,N>& b,usize offset,u64 value,usize size)
{ for (usize n=0;n<size;++n) { b[offset+n]=std::byte(value&255); value>>=8; } }
template <class C, class O> void test()
{
 static_assert(C::frame_bytes==O::frame_bytes && C::context_offset==O::context && C::fp_offset==O::fp);
 static_assert(C::mask_offset==O::mask && C::stack_offset==O::stack && C::stack_size_offset==O::stack_size);
 static_assert(C::fcsr_offset==O::fcsr && C::reserved_offset==O::reserved && C::magic_offset==O::magic && C::record_size_offset==O::record_size);
 static_assert(O::frame_alignment==16 && O::fp_union_bytes==528 && O::fp_d_bytes==264 && O::info_bytes==128);
 static_assert(O::register_bytes==32*C::word_bytes && C::info_pid_offset==O::info_pid && O::info_uid==O::info_pid+4);
 constexpr u64 begin=0x40000000,end=0x80000000;
 typename C::Frame frame{};
 for (usize i=0;i<32;++i) frame.registers[i]=static_cast<uptr>(0x10203040+i*0x10001);
 frame.pc=begin+0x102;frame.sp=end-1;frame.flags=0xdeadbeef;frame.fs_base=0x1234;frame.gs_base=0x9876;
 for (usize i=0;i<512;++i) frame.fp_state[i]=static_cast<u8>(i*37+19);
 frame.fp_state[256]=0xab;
 SignalMask mask;mask.words={0xfedcba9876543210ULL,0xdeadbeef};
 typename C::Info info{12,-6,0x1020304,0x5060708};
 typename C::Stack stack{begin+4096,16384,0x80000000};
 typename C::Image encoded{};
 CHECK(C::encode(frame,mask,stack,info,begin,end,encoded));
 std::array<std::byte,O::frame_bytes> expected{};
 wire(expected,0,12,4);wire(expected,8,0xfffffffa,4);wire(expected,O::info_pid,info.sender_pid,4);wire(expected,O::info_uid,info.sender_uid,4);
 wire(expected,O::stack,stack.base,C::word_bytes);wire(expected,O::stack_flags,stack.flags,4);wire(expected,O::stack_size,stack.size,C::word_bytes);
 wire(expected,O::mask,mask.words[0],8);wire(expected,O::pc,frame.pc,C::word_bytes);
 for (usize i=1;i<32;++i) wire(expected,O::context+i*C::word_bytes,i==2?frame.sp:frame.registers[i],C::word_bytes);
 for (usize i=0;i<256;++i) expected[O::fp+i]=std::byte(frame.fp_state[i]);
 wire(expected,O::fcsr,0xab,4);
 CHECK(encoded.bytes==expected); // Entire independent source-derived image, including all padding.
 typename C::Placement place{123};
 CHECK(C::place(end-1,begin,end,place));CHECK(place.address==((end-1-O::frame_bytes)&~u64{15}));
 auto placement_before=place;CHECK(!C::place(begin+O::frame_bytes-1,begin,end,place));CHECK(place.address==placement_before.address);
 CHECK(!C::place(0,begin,end,place));CHECK(!C::place(end+1,begin,end,place));CHECK(!C::place(~u64{},begin,end,place));
 CHECK(!C::place(begin,begin,begin,place));CHECK(!C::place(begin,begin+1,begin,place));
 for (u64 n=0;n<2048;++n) {
  typename C::Placement p{};const auto top=begin+O::frame_bytes+n;
  const bool good=C::place(top,begin,end,p);const auto reference=(top-O::frame_bytes)&~u64{15};
  CHECK(good==(reference>=begin));if(good){CHECK(p.address==reference);CHECK(p.address+O::frame_bytes<=top);}
 }
 typename C::Frame active{};
 CHECK(C::stage_handler(frame,place,begin+0x200,begin+0x300,info,begin,end,active));
 CHECK(active.pc==begin+0x200 && active.sp==place.address && active.registers[2]==place.address);
 CHECK(active.registers[1]==begin+0x300 && active.registers[10]==12 && active.registers[11]==place.address && active.registers[12]==place.address+128);
 CHECK(active.registers[4]==frame.registers[4] && active.flags==frame.flags && active.registers[0]==0);
 const auto active_before=active;
 auto bad_delivery=[&](u64 pc,u64 restorer,typename C::Placement p,typename C::Info in){CHECK(!C::stage_handler(frame,p,pc,restorer,in,begin,end,active));CHECK(std::memcmp(&active,&active_before,sizeof(active))==0);};
 bad_delivery(begin+1,begin+0x300,place,info);bad_delivery(begin,end-4,place,info);bad_delivery(begin,begin,{place.address+1},info);bad_delivery(begin,begin,place,{65,0,0,0});bad_delivery(begin,begin,place,{12,-1,0,0});
 auto pristine=expected;unsigned reads=0;bool fail=false;
 auto read=[&](u64 address,std::span<std::byte> output){++reads;CHECK(address==place.address);CHECK(output.size()==O::frame_bytes);std::memcpy(output.data(),expected.data(),output.size());return !fail;};
 typename C::Restored restored{};restored.frame.pc=0x7777;
 CHECK(C::decode(place.address,active,begin,end,read,restored));
 CHECK(restored.frame.pc==frame.pc && restored.frame.sp==frame.sp && restored.frame.registers[2]==frame.sp);
 CHECK(restored.frame.registers[4]==frame.registers[4] && restored.frame.registers[0]==0);
 CHECK(restored.frame.flags==active.flags && restored.frame.fs_base==active.fs_base && restored.frame.gs_base==active.gs_base);
 CHECK(restored.mask.words[0]==(mask.words[0]&~((u64{1}<<8)|(u64{1}<<18))) && !restored.mask.words[1]);
 CHECK(restored.stack.base==stack.base && restored.stack.size==stack.size && restored.stack.flags==stack.flags);
 for(usize i=0;i<256;++i)CHECK(restored.frame.fp_state[i]==frame.fp_state[i]);
 CHECK(restored.frame.fp_state[256]==0xab);for(usize i=257;i<512;++i)CHECK(restored.frame.fp_state[i]==0);
 wire(expected,O::tp,0x31415926,C::word_bytes);wire(expected,O::pc,begin+0x442,C::word_bytes);wire(expected,O::sp,end-7,C::word_bytes);
 wire(expected,O::fcsr,0xffffffff,4);wire(expected,O::stack_flags,0xdeadbeef,4);
 // Linux ignores these fields/padding, and stages raw altstack data for policy.
 for(usize i=128;i<O::stack;++i)expected[i]=std::byte{0xef};
 for(usize i=O::mask+8;i<O::context;++i)expected[i]=std::byte{0xaa};
 for(usize i=O::fcsr+4;i<O::reserved;++i)expected[i]=std::byte{0xb5};
 CHECK(C::decode(place.address,active,begin,end,read,restored));
 CHECK(restored.frame.registers[4]==0x31415926 && restored.frame.pc==begin+0x442 && restored.frame.sp==end-7);
 CHECK(restored.frame.fp_state[256]==255 && restored.stack.flags==0xdeadbeef && restored.frame.flags==frame.flags);
 expected=pristine;
 const auto restored_before=restored;
 auto invalid=[&](){CHECK(!C::decode(place.address,active,begin,end,read,restored));CHECK(std::memcmp(&restored,&restored_before,sizeof(restored))==0);};
 for(usize offset:{O::reserved,O::magic,O::record_size})for(usize byte=0;byte<4;++byte){expected=pristine;expected[offset+byte]=std::byte{1};invalid();}
 expected=pristine;wire(expected,O::magic,0x53465457,4);wire(expected,O::record_size,128,4);invalid();
 for(u64 pc:{u64{0},begin-2,begin+1,end,~u64{}}){expected=pristine;wire(expected,O::pc,pc,C::word_bytes);invalid();}
 for(u64 sp:{u64{0},begin,end+1}){expected=pristine;wire(expected,O::sp,sp,C::word_bytes);invalid();}
 expected=pristine;fail=true;invalid();fail=false;
 for(u64 sp:{u64{0},begin-1,end-O::frame_bytes+1,~u64{}}){const auto before_reads=reads;CHECK(!C::decode(sp,active,begin,end,read,restored));CHECK(reads==before_reads);}
 auto image_before=encoded;
 frame.pc|=1;CHECK(!C::encode(frame,mask,stack,info,begin,end,encoded));CHECK(encoded.bytes==image_before.bytes);frame.pc&=~uptr{1};
 if constexpr(C::word_bytes==4){frame.registers[4]=u64{1}<<32;CHECK(!C::encode(frame,mask,stack,info,begin,end,encoded));frame.registers[4]=0;stack.base=u64{1}<<32;CHECK(!C::encode(frame,mask,stack,info,begin,end,encoded));CHECK(!C::place(u64{1}<<32,0,u64{1}<<32,place));}
}
int main(){test<Rv32SignalFrame,linux_riscv_oracle::rv32>();test<Rv64SignalFrame,linux_riscv_oracle::rv64>();std::printf("PASS RISC-V RT frame model: %u checks; no target execution\n",checks);}
