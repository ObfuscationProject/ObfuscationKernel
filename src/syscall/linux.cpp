#include "ok/syscall/linux.hpp"

namespace ok::syscall
{

Request LinuxSyscallAbi::decode_x86_64(const LinuxSyscallFrame &frame, sched::ProcessId caller) const
{
    return Request{
        .number = static_cast<Number>(frame.syscall_number),
        .caller = caller,
        .args = {frame.rdi, frame.rsi, frame.rdx, frame.r10, frame.r8, frame.r9},
    };
}

i64 ErrnoMapper::errno_for(Status status)
{
    switch (status.code())
    {
    case StatusCode::ok:
        return 0;
    case StatusCode::invalid_argument:
        return linux_EINVAL;
    case StatusCode::no_memory:
        return linux_ENOMEM;
    case StatusCode::not_found:
        return linux_ENOENT;
    case StatusCode::already_exists:
        return linux_EEXIST;
    case StatusCode::busy:
    case StatusCode::would_block:
        return linux_EAGAIN;
    case StatusCode::denied:
        return linux_EACCES;
    case StatusCode::unsupported:
        return linux_ENOSYS;
    case StatusCode::overflow:
        return linux_EOVERFLOW;
    case StatusCode::fault:
        return linux_EFAULT;
    case StatusCode::interrupted:
        return linux_EAGAIN;
    case StatusCode::not_initialized:
        return linux_EIO;
    }
    return linux_EIO;
}

i64 ErrnoMapper::result_for(Response response)
{
    if (response.status.ok())
    {
        return response.value;
    }
    return -errno_for(response.status);
}

i64 LinuxSyscallDispatcher::dispatch_x86_64(LinuxSyscallFrame &frame, sched::ProcessId caller) const
{
    if (table_ == nullptr)
    {
        frame.return_value = -linux_ENOSYS;
        return frame.return_value;
    }
    const auto request = abi_.decode_x86_64(frame, caller);
    const auto response = table_->dispatch(request);
    const auto value = ErrnoMapper::result_for(response);
    abi_.encode_return(frame, value);
    return value;
}

Status LinuxAuxvBuilder::add(u64 type, u64 value)
{
    return entries_.push_back(LinuxAuxvEntry{.type = type, .value = value});
}

} // namespace ok::syscall
