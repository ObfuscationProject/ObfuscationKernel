#include "ok/arch/mips64/signal.hpp"
#include <cstddef>
using namespace ok;
using C=ok::user::native::Mips64N64SignalFrame;
static_assert(sizeof(uptr)==8);
static_assert(sizeof(C::Frame)==816);
static_assert(offsetof(C::Frame,fp_state)==304);
extern "C" bool n64_encode(const C::Frame& f,C::Image& image)
{
 return C::encode(f,{C::FpContract::eager_fr1_legacy_nan_no_extensions,C::ResumeContract::normalized_resume_pc},{},{},{16,-6,1,1},0x40000000,0x80000000,image);
}
extern "C" bool n64_decode(const C::Frame& f,const C::Image& image,C::ReturnPlan& plan)
{
 return C::decode(0x70000000,f,{C::FpContract::eager_fr1_legacy_nan_no_extensions,C::ResumeContract::normalized_resume_pc},0x40000000,0x80000000,[&](u64,std::span<std::byte> target){for(usize i=0;i<target.size();++i)target[i]=image.bytes[i];return true;},plan);
}
