#include "ok/user/native_signal_frame_aarch64.hpp"
using namespace ok;
using C=user::native::Aarch64SignalFrame;
extern "C" bool aarch64_frame_compile(const C::Frame &current,const std::byte *input,C::Image &image,C::Restored &out)
{
    C::Placement p;
    if(!C::place(current.sp,0x10000000,0x40000000,p))return false;
    if(!C::encode(current,{}, {},{10,0,1,2},0x10000000,0x40000000,image))return false;
    if(!C::decode(p.address,current,0x10000000,0x40000000,[&](u64,std::span<std::byte> bytes){for(usize i=0;i<bytes.size();++i)bytes[i]=input[i];return true;},out))return false;
    C::Frame handler;
    return C::setup_handler(current,p,{10,0,1,2},0x10001000,0x10002000,true,0x10000000,0x40000000,handler);
}
