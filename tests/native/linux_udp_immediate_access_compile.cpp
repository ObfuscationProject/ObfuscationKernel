#include "../../src/user/linux_udp_immediate_access.hpp"

using namespace ok;
constexpr auto profile = abi::linux_user::configured_native_profile();
using Access = user::native::LinuxUdpImmediateAccess<profile>;
template class user::native::LinuxUdpImmediateAccess<profile>;

extern "C" __attribute__((used, noinline)) bool linux_udp_immediate_live(const Access &access)
{ return access.live(); }
extern "C" __attribute__((used, noinline)) u64 linux_udp_immediate_raw(const Access &access)
{ return access.raw_syscall(); }
extern "C" __attribute__((used, noinline)) Status linux_udp_immediate_copy(
    Access &access, uptr pointer, std::span<std::byte> data)
{
    if (auto status = access.range(pointer, data.size(), false); !status) return status;
    return access.read(pointer, data);
}
extern "C" __attribute__((used, noinline)) Status linux_udp_immediate_publish(
    Access &access, uptr pointer, std::span<const std::byte> data)
{ return access.write(pointer, data); }
