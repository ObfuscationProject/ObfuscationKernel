#pragma once
#include <ok/abi/linux_tcp_data.hpp>
#include <ok/abi/linux_tcp_accept_call.hpp>
#include <ok/abi/linux_native.hpp>
#include <ok/abi/linux_udp_call.hpp>
#include <ok/net/native_tcp_stream.hpp>
#include <ok/sched/scheduler.hpp>
#include <ok/syscall/syscall.hpp>
#include <ok/uapi/syscall.h>
#include <ok/user/unix_pathname_operation.hpp>

namespace ok::user::native
{
[[nodiscard]] inline bool canonical_tcp_fd(const sched::SchedulerState &scheduler,
    const sched::ProcessControlBlock &process, u64 fd)
{
    if (fd > static_cast<u64>(__INT32_MAX__)) return false;
    const auto *slot = process.fd_table().entry(static_cast<i32>(fd));
    const auto *description = slot && slot->used ? scheduler.open_description(slot->description) : nullptr;
    const auto *service = scheduler.native_tcp_service();
    return description && description->references && description->native_tcp_handle && service &&
        service->valid(description->native_tcp_handle) && !description->native_udp_handle && !description->native_shared_file && !description->unix_endpoint && !description->node &&
        description->pipe == sched::invalid_description && description->unix_write_pipe == sched::invalid_description &&
        !description->console && !description->input_console;
}

[[nodiscard]] inline abi::linux_user::TcpCall native_tcp_call(
    const arch::execution::Syscall &call, const sched::SchedulerState &scheduler,
    const sched::ProcessControlBlock &process)
{
    if (native_unix_path_call(call, scheduler, process) != abi::linux_user::UnixPathCall::none) return abi::linux_user::TcpCall::none;
    using Call = abi::linux_user::TcpCall;
    using Number = syscall::Number;
    if (call.number == static_cast<uptr>(Number::socket))
        return (call.arguments[1] & 0xf) == OK_SOCK_STREAM ? Call::socket : Call::none;
    if (call.number == static_cast<uptr>(Number::bind))
        return canonical_tcp_fd(scheduler, process, call.arguments[0]) ? Call::bind : Call::none;
    if (call.number == static_cast<uptr>(Number::read))
        return canonical_tcp_fd(scheduler, process, call.arguments[0]) ? Call::read : Call::none;
    if (call.number == static_cast<uptr>(Number::write))
        return canonical_tcp_fd(scheduler, process, call.arguments[0]) ? Call::write : Call::none;
    if (call.number == static_cast<uptr>(Number::sendto))
        return canonical_tcp_fd(scheduler, process, call.arguments[0]) ? Call::sendto : Call::none;
    if (call.number == static_cast<uptr>(Number::recvfrom))
        return canonical_tcp_fd(scheduler, process, call.arguments[0]) ? Call::recvfrom : Call::none;
    if (call.number == static_cast<uptr>(Number::connect)) return Call::connect;
    if (call.number == static_cast<uptr>(Number::listen)) return Call::listen;
    if (call.number == static_cast<uptr>(Number::accept)) return Call::accept;
    if (call.number == static_cast<uptr>(Number::accept4)) return Call::accept4;
    if (call.number == static_cast<uptr>(Number::shutdown)) return Call::shutdown;
    if (call.number == static_cast<uptr>(Number::getsockname)) return Call::getsockname;
    if (call.number == static_cast<uptr>(Number::getpeername)) return Call::getpeername;
    if (call.number == static_cast<uptr>(Number::getsockopt)) return Call::getsockopt;
    return Call::none;
}

template <abi::linux_user::Profile Selected> requires abi::linux_user::SupportedProfile<Selected>
[[nodiscard]] abi::linux_user::TcpCall linux_tcp_direct_call(const arch::execution::UserFrame &frame,
    const sched::SchedulerState &scheduler, const sched::ProcessControlBlock &process)
{
    namespace lu = abi::linux_user;
    if (linux_unix_path_call<Selected>(frame, scheduler, process) != lu::UnixPathCall::none) return lu::TcpCall::none;
    const auto route = lu::TcpAcceptCall<Selected>::route(lu::NativeBridge<Selected>::snapshot(frame));
    auto decoded = lu::NativeBridge<Selected>::decode_call(frame, route.socketcall ? 2 : 3);
    if (!decoded) return lu::TcpCall::none;
    const auto &call = decoded.value();
    const auto operation = route.operation;
    using Call = lu::TcpCall;
    if (operation == Call::socket)
        return (call.arguments[1] & lu::UdpConstants<Selected>::sock_type_mask) ==
            lu::UdpConstants<Selected>::sock_stream ? Call::socket : Call::none;
    // Only selectors 5/18 reserve a TCP copied accept route. The register-only
    // classifier cannot read the fd array or infer a provider from its fd.
    if (route.socketcall) return operation;
    if (operation == Call::bind || operation == Call::connect || operation == Call::getsockname ||
        operation == Call::getpeername || operation == Call::read || operation == Call::write ||
        operation == Call::readv || operation == Call::writev || operation == Call::sendto || operation == Call::recvfrom)
        return canonical_tcp_fd(scheduler, process, call.arguments[0]) ? operation : Call::none;
    return operation;
}
}
