// Pure ten-target family/opcode/frame models; no captured authority or target execution.
#include "../../src/user/native_syscall_origin.hpp"
#include <cstdio>
#include <cstdlib>
#include <source_location>
using namespace ok;
namespace lu=abi::linux_user;
namespace hw=arch::execution;
namespace native=user::native;
static usize checks{}, profiles{}, copies{};
void check(bool valid, std::source_location at=std::source_location::current())
{
    ++checks;
    if(!valid) { std::fprintf(stderr,"UDP_RESTART_FRAME_FAIL line=%u checks=%zu\n",at.line(),checks); std::abort(); }
}
template <lu::Profile P> void reg(hw::UserFrame &frame, usize index, u64 value)
{
    if constexpr(P==lu::Profile::i386)
    { constexpr usize map[]{7,6,5,4,0,2,1,0}; frame.registers[map[index]]=value; }
    else if constexpr(P==lu::Profile::x86_64)
    { constexpr usize map[]{14,11,12,13,10,9,8,0,7,6,5,4,3,2,1,0}; frame.registers[map[index]]=value; }
    else frame.registers[index]=value;
}
template <lu::Profile P> void exercise(std::span<const u8> code, hw::Trap trap, bool thumb=false)
{
    using A=native::RestartAdapter<P>;
    using Origins=native::SyscallOriginTable<P>;
    using C=lu::UdpConstants<P>;
    using Codec=lu::Codec<P>;
    using Binding=typename Origins::LinuxUdpReceiveBinding;
    using Permit=typename Origins::LinuxUdpRestartPermit;
    static_assert(Origins::template supported_binding<Binding>);
    static_assert(!std::same_as<Permit,typename Origins::RestartPermit>);
    static_assert(!std::same_as<Permit,typename Origins::LinuxUdpReceiveBorrow>);
    static_assert(!std::is_constructible_v<Permit,hw::UserFrame,hw::Trap,typename A::Plan>);
    static_assert(!requires(Origins &table, const typename Origins::Identity &identity,
                           const hw::UserFrame &frame, const Binding &binding) {
        table.bind(identity,frame,binding);
    });
    constexpr uptr begin=0x10000000,end=0x20000000,site=begin+0x2000;
    hw::UserFrame frame{};
    for(usize i=0;i<frame.registers.size();++i) frame.registers[i]=0x4100+i*31;
    for(usize i=0;i<frame.fp_state.size();++i) frame.fp_state[i]=static_cast<u8>(i*11+0x29);
    frame.pc=site+code.size(); frame.sp=0x18004000; frame.fs_base=0x18001000; frame.gs_base=0x18002000;
    frame.flags=P==lu::Profile::arm32 ? (thumb ? 0x30 : 0x10) : 0x1234;
    if constexpr(P==lu::Profile::arm32) frame.registers[15]=frame.pc;
    auto number=[&](hw::UserFrame &target,u64 value) { reg<P>(target,Codec::profile.number_register,value); };
    auto selector=[&](hw::UserFrame &target,u64 value) { reg<P>(target,static_cast<usize>(Codec::profile.argument_registers[0]),value); };
    auto candidate=[&](const hw::UserFrame &captured) {
        check(A::family(captured)==A::Family::linux_udp_receive);
        const auto result=A::prepare(captured,trap,code,begin,end); check(result.ok());
        auto expected=captured; expected.pc=site;
        if constexpr(P==lu::Profile::arm32) expected.registers[15]=site;
        if constexpr(P==lu::Profile::ppc) expected.fp_state[lu::NativeBridge<P>::ppc_condition_offset]|=0x10;
        check(Origins::same_frame(result.value().frame,expected));
        check(result.value().site.instruction==site && result.value().site.bytes==code.size() && result.value().site.thumb==thumb);
        for(usize i=0;i<code.size();++i) check(result.value().opcode[i]==code[i]);
        const auto before=copies;
        const auto copied=A::prepare_copy(captured,trap,begin,end,[&](uptr address,std::span<u8> bytes)->Status {
            ++copies; check(address==site && bytes.size()==code.size());
            for(usize i=0;i<bytes.size();++i) bytes[i]=code[i];
            return {};
        });
        check(copied && copies==before+1 && Origins::same_frame(copied.value().frame,expected));
        constexpr std::array<u8,4> wrong{};
        check(!A::prepare(captured,trap,std::span<const u8>{wrong}.first(code.size()),begin,end));
        auto changed_trap=trap; changed_trap.kind=hw::TrapKind::timer;
        check(!A::prepare(captured,changed_trap,code,begin,end));
        if constexpr(P==lu::Profile::mips || P==lu::Profile::mips64)
        { changed_trap=trap; changed_trap.error_code|=0x80000000ULL; check(!A::prepare(captured,changed_trap,code,begin,end)); }
        auto changed=captured; changed.pc=begin;
        if constexpr(P==lu::Profile::arm32) changed.registers[15]=changed.pc;
        check(!A::prepare(changed,trap,code,begin,end));
        if constexpr(Codec::profile.word_bytes==4)
        {
            changed=captured; changed.registers[21]|=uptr{1}<<32;
            check(!A::prepare(changed,trap,code,begin,end));
        }
        const auto copied_before=copies;
        const auto refused=A::prepare_copy(captured,trap,begin,end,[&](uptr,std::span<u8>)->Status {
            ++copies; return Status::fault("model executable span refusal");
        });
        check(!refused && copies==copied_before+1);
        check(captured.pc==site+code.size());
    };
    if(const auto direct=C::syscall_number(lu::UdpCall::recvfrom))
    { auto captured=frame; number(captured,*direct); candidate(captured); }
    for(auto operation : {lu::UdpCall::socket,lu::UdpCall::bind,lu::UdpCall::sendto})
        if(const auto direct=C::syscall_number(operation))
        { auto captured=frame; number(captured,*direct); check(A::family(captured)==A::Family::none); }
    if(const auto socketcall=C::socketcall_number())
    {
        auto captured=frame; number(captured,*socketcall); selector(captured,12); candidate(captured);
        for(u64 operation : std::array<u64,6>{0,1,2,11,13,~u64{0}})
        { selector(captured,operation & Codec::word_mask); check(A::family(captured)==A::Family::none); }
    }
    auto unrelated=frame; number(unrelated,123456); check(A::family(unrelated)==A::Family::none);
    check(!A::prepare(unrelated,trap,code,begin,end));
    ++profiles;
}
int main()
{
    using K=hw::TrapKind;
    constexpr u8 x64[]{0x0f,0x05},x32[]{0xcd,0x80},a64[]{1,0,0,0xd4},arm[]{0,0,0,0xef},thumb[]{0,0xdf};
    constexpr u8 rv[]{0x73,0,0,0},la[]{0,0,0x2b,0},mips[]{0,0,0,12},ppc[]{0x44,0,0,2};
    exercise<lu::Profile::i386>(x32,{K::syscall,128});
    exercise<lu::Profile::x86_64>(x64,{K::syscall,256});
    exercise<lu::Profile::aarch64>(a64,{K::syscall,0x15,0x54000000});
    exercise<lu::Profile::arm32>(arm,{K::syscall,2});
    exercise<lu::Profile::arm32>(thumb,{K::syscall,2},true);
    exercise<lu::Profile::rv64>(rv,{K::syscall,8,8});
    exercise<lu::Profile::rv32>(rv,{K::syscall,8,8});
    exercise<lu::Profile::loongarch64>(la,{K::syscall,11});
    exercise<lu::Profile::mips>(mips,{K::syscall,8,32});
    exercise<lu::Profile::mips64>(mips,{K::syscall,8,32});
    exercise<lu::Profile::ppc>(ppc,{K::syscall,8});
    check(profiles==11); // All ten profiles and the separate ARM Thumb variant.
    std::printf("LINUX_UDP_RESTART_FRAME_PASS checks=%zu profiles=10 variants=%zu opcode_copies=%zu captured_authority=0 hosted=1 guest=0 native_restart=0\n",checks,profiles,copies);
}
