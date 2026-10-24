#pragma once
#include "ok/abi/linux_unix_pathname.hpp"
#include "ok/abi/linux_native.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/syscall/syscall.hpp"

namespace ok::user::native
{
inline bool unix_path_fd(const sched::SchedulerState &scheduler, const sched::ProcessControlBlock &process, u64 fd)
{
    if (fd > static_cast<u64>(__INT32_MAX__)) return false;
    const auto *slot = process.fd_table().entry(static_cast<i32>(fd));
    return slot && slot->used && scheduler.is_unix_socket(slot->description);
}
inline abi::linux_user::UnixPathCall native_unix_path_call(const arch::execution::Syscall &call,
    const sched::SchedulerState &scheduler, const sched::ProcessControlBlock &process)
{
    using C = abi::linux_user::UnixPathCall;
    using N = syscall::Number;
    if (call.number == static_cast<uptr>(N::socket)) return call.arguments[0] == 1 ? C::socket : C::none;
    if (!unix_path_fd(scheduler, process, call.arguments[0])) return C::none;
    if (call.number == static_cast<uptr>(N::bind)) return C::bind;
    if (call.number == static_cast<uptr>(N::connect)) return C::connect;
    if (call.number == static_cast<uptr>(N::listen)) return C::listen;
    if (call.number == static_cast<uptr>(N::accept)) return C::accept;
    if (call.number == static_cast<uptr>(N::accept4)) return C::accept4;
    if (call.number == static_cast<uptr>(N::getsockopt)) return C::getsockopt;
    return C::none;
}
template <abi::linux_user::Profile Selected> requires abi::linux_user::SupportedProfile<Selected>
abi::linux_user::UnixPathCall linux_unix_path_call(const arch::execution::UserFrame &frame,
    const sched::SchedulerState &scheduler, const sched::ProcessControlBlock &process)
{
    namespace lu = abi::linux_user;
    using C = lu::UnixPathCall; using T = lu::TcpCall;
    const auto saved = lu::NativeBridge<Selected>::decode_call(frame, 3);
    if (!saved) return C::none;
    const auto &call = saved.value();
    const auto operation = lu::tcp_call<Selected>(call.number);
    if (operation == T::socket) return call.arguments[0] == 1 ? C::socket : C::none;
    if (!unix_path_fd(scheduler, process, call.arguments[0])) return C::none;
    switch (operation)
    {
    case T::bind: return C::bind;
    case T::connect: return C::connect;
    case T::listen: return C::listen;
    case T::accept: return C::accept;
    case T::accept4: return C::accept4;
    case T::getsockopt: return C::getsockopt;
    default: return C::none;
    }
}
}
