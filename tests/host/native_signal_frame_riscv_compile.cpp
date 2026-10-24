#include "ok/user/native_signal_frame_riscv.hpp"
using namespace ok; using namespace ok::user::native;
#if __riscv_xlen == 32
using C=Rv32SignalFrame;
static_assert(__builtin_offsetof(C::Frame,fp_state)==160);
#else
using C=Rv64SignalFrame;
static_assert(__builtin_offsetof(C::Frame,fp_state)==304);
#endif
static_assert(sizeof(uptr)==C::word_bytes);
extern "C" bool riscv_frame_compile(C::Frame &frame,C::Image &image,C::Restored &restored)
{
 C::Placement p;
 if(!C::place(frame.sp,0x40000000,0x80000000,p))return false;
 if(!C::encode(frame,{}, {},{10,-6,1,2},0x40000000,0x80000000,image))return false;
 if(!C::stage_handler(frame,p,0x40001000,0x40002000,{10,-6,1,2},0x40000000,0x80000000,frame))return false;
 return C::decode(p.address,frame,0x40000000,0x80000000,[&](u64,std::span<std::byte> out){for(usize i=0;i<out.size();++i)out[i]=image.bytes[i];return true;},restored);
}
