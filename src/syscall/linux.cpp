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

Result<LinuxInitialStack> LinuxInitialStackBuilder::build(uptr stack_top, std::span<const std::string_view> argv,
                                                          std::span<const std::string_view> envp,
                                                          std::span<const LinuxAuxvEntry> auxv)
{
    if (stack_top == 0)
    {
        return Status::invalid_argument("Linux initial stack top is zero");
    }
    if (argv.size() > 16 || envp.size() > 16)
    {
        return Status::overflow("Linux initial stack argument table is full");
    }
    const usize required_words = 1 + argv.size() + 1 + envp.size() + 1 + auxv.size() * 2 + 2;
    if (required_words > words_.size())
    {
        return Status::overflow("Linux initial stack word table is full");
    }

    std::array<uptr, 16> argv_addresses{};
    std::array<uptr, 16> envp_addresses{};
    uptr string_cursor = stack_top;
    for (usize i = argv.size(); i != 0; --i)
    {
        const auto index = i - 1;
        string_cursor -= static_cast<uptr>(argv[index].size() + 1);
        argv_addresses[index] = string_cursor;
    }
    for (usize i = envp.size(); i != 0; --i)
    {
        const auto index = i - 1;
        string_cursor -= static_cast<uptr>(envp[index].size() + 1);
        envp_addresses[index] = string_cursor;
    }

    constexpr uptr stack_alignment = 16;
    const auto words_bytes = static_cast<uptr>(required_words * sizeof(u64));
    const auto unaligned_stack = string_cursor > words_bytes ? string_cursor - words_bytes : 0;
    const auto stack_pointer = unaligned_stack & ~static_cast<uptr>(stack_alignment - 1u);
    if (stack_pointer == 0)
    {
        return Status::overflow("Linux initial stack does not fit");
    }

    word_count_ = 0;
    words_[word_count_++] = static_cast<u64>(argv.size());
    const auto argv_pointer = stack_pointer + static_cast<uptr>(word_count_ * sizeof(u64));
    for (usize i = 0; i < argv.size(); ++i)
    {
        words_[word_count_++] = argv_addresses[i];
    }
    words_[word_count_++] = 0;
    const auto envp_pointer = stack_pointer + static_cast<uptr>(word_count_ * sizeof(u64));
    for (usize i = 0; i < envp.size(); ++i)
    {
        words_[word_count_++] = envp_addresses[i];
    }
    words_[word_count_++] = 0;
    const auto auxv_pointer = stack_pointer + static_cast<uptr>(word_count_ * sizeof(u64));
    for (const auto &entry : auxv)
    {
        words_[word_count_++] = entry.type;
        words_[word_count_++] = entry.value;
    }
    words_[word_count_++] = 0;
    words_[word_count_++] = 0;

    return LinuxInitialStack{
        .stack_pointer = stack_pointer,
        .argc = static_cast<u64>(argv.size()),
        .argv_pointer = argv_pointer,
        .envp_pointer = envp_pointer,
        .auxv_pointer = auxv_pointer,
        .words = word_count_,
    };
}

} // namespace ok::syscall
