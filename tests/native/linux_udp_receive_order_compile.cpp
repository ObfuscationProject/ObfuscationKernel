#include "../../src/user/linux_udp_receive_order.hpp"
#include "ok/abi/linux_native.hpp"
using namespace ok;
struct LinuxUdpCompileAccess
{
    bool live() const;
    Status read(u64,std::span<std::byte>);
    Status write(u64,std::span<const std::byte>);
};
Result<usize> linux_udp_receive_order_compile_probe(user::native::linux_udp::ReceiveScratch &scratch,
    net::NativeUdpService &service,u32 endpoint,LinuxUdpCompileAccess &access,user::native::linux_udp::ReceiveOutput output)
{
    return scratch.receive<abi::linux_user::configured_native_profile()>(service,endpoint,access,output);
}
