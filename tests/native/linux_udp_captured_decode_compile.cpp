#include "../../src/user/linux_udp_captured_decode.hpp"
#include "../../src/user/linux_udp_immediate_access.hpp"

using namespace ok;
constexpr auto profile = abi::linux_user::configured_native_profile();
using Access = user::native::LinuxUdpImmediateAccess<profile>;
using Origins = user::native::SyscallOriginTable<profile>;

extern "C" __attribute__((used, noinline)) Result<abi::linux_user::UdpRequest> linux_udp_captured_decode(
    const arch::execution::UserFrame &frame, Access &access)
{ return user::native::linux_udp::decode_captured<profile>(frame, access); }

extern "C" __attribute__((used, noinline)) bool linux_udp_captured_acquire_decode(
    Origins &origins, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
    abi::linux_user::UdpRequest &output)
{
    auto access = Access::acquire(origins, process, thread);
    if (!access) return false;
    auto request = user::native::linux_udp::decode_captured<profile>(thread.native_frame, access.value());
    if (!request) return false;
    output = request.value();
    return true;
}
