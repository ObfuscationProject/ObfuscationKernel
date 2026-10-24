#include <ok/net/tcp_stream_checked.hpp>
#include <ok/net/native_tcp_ethernet.hpp>
#include <type_traits>
using namespace ok;
using namespace ok::net;
static_assert(!std::is_copy_constructible_v<NativeTcpStreamService>);
static_assert(!std::is_move_constructible_v<TcpStreamWaits>);
static_assert(!std::is_copy_constructible_v<TcpStreamOperations>);
static_assert(!std::is_move_constructible_v<NativeTcpEthernetExecutor>);
struct CopiedDevice
{
    Result<NativeUdpTransmitState> reap(usize) { return NativeUdpTransmitState{}; }
    Result<usize> receive(std::span<std::byte>) { return Status::would_block(); }
    Status try_transmit(wire::Bytes) { return Status::would_block(); }
};
struct Copy
{
    Status preflight(u64, usize, bool) { return {}; }
    Status read(u64, std::span<std::byte>) { return Status::fault(); }
    Status write(u64, wire::Bytes) { return Status::fault(); }
};
struct Iss { Result<u32> operator()(tcp::Endpoint, tcp::Endpoint) { return u32{17}; } };
extern "C" void tcp_stream_native_object(sched::SchedulerState &scheduler, NativeTcpStreamService &service,
    TcpStreamWaits &waits, NativeUdpService &udp, NativeTcpEthernetExecutor &executor, u64 pid)
{
    TcpStreamOperations operations{scheduler, service}; Copy copy; Iss iss; CopiedDevice device;
    static_cast<void>(scheduler.set_native_tcp_service(service));
    static_cast<void>(operations.open(pid));
    static_cast<void>(operations.read(pid, 3, 4096, 5, copy));
    static_cast<void>(operations.write(pid, 3, 4096, 5, copy));
    static_cast<void>(waits.arm(pid, 3, tcp::Readiness::read_interest));
    static_cast<void>(executor.poll(service, udp, waits, device, iss, service.now()));
}
template <abi::linux_user::Profile Profile> void profile_object(TcpStreamOperations &operations, u64 pid)
{
    LinuxTcpStreamAdapter<Profile> adapter{operations}; Copy copy;
    static_cast<void>(adapter.socket(pid, 2, abi::linux_user::UdpConstants<Profile>::sock_stream, 6));
    static_cast<void>(adapter.bind(pid, 3, 4096, 16, copy));
    static_cast<void>(adapter.connect(pid, 3, 4096, 16, copy, 23, 0));
    static_cast<void>(adapter.read(pid, 3, 4096, 3, copy));
    static_cast<void>(adapter.write(pid, 3, 4096, 3, copy));
    abi::linux_user::RegisterFile frame{};
    static_cast<void>(adapter.encode_result(frame, Status::would_block()));
}
template void profile_object<abi::linux_user::Profile::i386>(TcpStreamOperations &, u64);
template void profile_object<abi::linux_user::Profile::x86_64>(TcpStreamOperations &, u64);
template void profile_object<abi::linux_user::Profile::aarch64>(TcpStreamOperations &, u64);
template void profile_object<abi::linux_user::Profile::arm32>(TcpStreamOperations &, u64);
template void profile_object<abi::linux_user::Profile::rv64>(TcpStreamOperations &, u64);
template void profile_object<abi::linux_user::Profile::rv32>(TcpStreamOperations &, u64);
template void profile_object<abi::linux_user::Profile::loongarch64>(TcpStreamOperations &, u64);
template void profile_object<abi::linux_user::Profile::mips>(TcpStreamOperations &, u64);
template void profile_object<abi::linux_user::Profile::mips64>(TcpStreamOperations &, u64);
template void profile_object<abi::linux_user::Profile::ppc>(TcpStreamOperations &, u64);
