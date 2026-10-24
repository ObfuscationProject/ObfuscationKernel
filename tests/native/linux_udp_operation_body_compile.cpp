#include "../../src/user/linux_udp_operation_body.hpp"
#include "../../src/user/linux_udp_immediate_access.hpp"
using namespace ok;
constexpr auto profile=abi::linux_user::configured_native_profile();
using Access=user::native::LinuxUdpImmediateAccess<profile>;
using Scratch=user::native::linux_udp::OperationScratch;
extern "C" __attribute__((used,noinline)) Status linux_udp_operation_body_kernel_probe(
    Scratch &scratch,Kernel &kernel,sched::ProcessControlBlock &process,Access &access,
    const abi::linux_user::Call &copied_raw,Status &failure,i64 &published)
{
    auto decode=[&]() -> Result<abi::linux_user::UdpRequest> {
        return abi::linux_user::UdpCallDecoder<profile>::decode_direct(copied_raw);
    };
    auto publish=[&](i64 result) -> Status {
        if(!access.live())return Status::denied("compile probe publication authority expired");
        published=result;return {};
    };
    return scratch.run<profile>(kernel,process,access,decode,publish,failure);
}
