// Hosted saved-frame/stack-copy model, not executed MIPS guest instructions.
#include "ok/abi/linux_stat_call.hpp"
#include "ok/abi/linux_native.hpp"
#include <cstdio>
#include <cstdlib>
using namespace ok;
using namespace ok::abi::linux_user;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);std::abort();}}while(false)
int main()
{
    using B=NativeBridge<Profile::mips>;
    using A=StatCallAdapter<Profile::mips>;
    B::Frame frame{};frame.sp=0x3000;frame.registers[2]=4366;
    frame.registers[4]=0xfeed0000ffffff9cull;
    frame.registers[5]=0x1000;frame.registers[6]=0x100;frame.registers[7]=0x7ff;
    for(u32 pointer : {0u,0x2000u,0xffffff80u})
    {
        unsigned copies=0;
        auto captured=B::decode_call_copying_stack(frame,A::argument_count(StatCall::statx),
            [&](uptr at,std::span<u8> out)->Status{
                ++copies;CHECK(at==0x3010&&out.size()==4);
                for(usize i=0;i<4;++i) out[i]=static_cast<u8>(pointer>>((3-i)*8));
                return {};
            });
        CHECK(copies==1&&captured&&captured.value().number==4366&&captured.value().argument_count==5);
        const auto &a=captured.value().arguments;CHECK(a[0]==0xffffff9c&&a[1]==0x1000&&a[2]==0x100&&a[3]==0x7ff&&a[4]==pointer);
        auto decoded=A::decode(StatCall::statx,std::span{a}.first(5));CHECK(decoded&&decoded.value().output==pointer&&decoded.value().fd==-100);
    }
    for(u64 sp : {0xfffffff0ull,0xffffffffull,0xffffffedull})
    {
        frame.sp=sp;unsigned copies=0;
        auto result=B::decode_call_copying_stack(frame,5,[&](uptr,std::span<u8>)->Status{++copies;return {};});
        CHECK(!result&&result.status().code()==StatusCode::fault&&copies==0);
    }
    frame.sp=0x3000;frame.registers[6]=0xffffffff;
    // An unreadable fifth argument precedes even invalid statx flags.
    unsigned calls=0;
    auto result=B::decode_call_copying_stack(frame,5,[&](uptr at,std::span<u8> bytes)->Status{
        ++calls;CHECK(at==0x3010&&bytes.size()==4);return Status::fault();});
    CHECK(!result&&result.status().code()==StatusCode::fault&&calls==1);
    std::puts("PASS hosted MIPS statx fifth-argument bridge; guest=0");
    std::printf("checks=%u\n",checks);
}
