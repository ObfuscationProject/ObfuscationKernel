#include "ok/core/shell.hpp"

#include "ok/core/kernel.hpp"

namespace ok
{
namespace
{

std::string_view trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    {
        value.remove_suffix(1);
    }
    return value;
}

} // namespace

Status KernelDebugShell::attach(Kernel &kernel)
{
    kernel_ = &kernel;
    return Status::success();
}

Result<std::string_view> KernelDebugShell::execute(std::string_view line)
{
    output_.clear();
    const auto command = trim(line);
    if (command.empty())
    {
        return output_.view();
    }
    Status status{};
    if (command == "help")
    {
        status = command_help();
    }
    else if (command == "status")
    {
        status = command_status();
    }
    else if (command == "mem")
    {
        status = command_memory();
    }
    else if (command == "ps")
    {
        status = command_processes();
    }
    else if (command == "drivers")
    {
        status = command_drivers();
    }
    else if (command == "fs")
    {
        status = command_filesystem();
    }
    else if (command == "posix")
    {
        status = command_posix();
    }
    else if (command == "test")
    {
        status = command_tests();
    }
    else if (command.starts_with("echo "))
    {
        status = command_echo(command.substr(5));
    }
    else
    {
        status = append("unknown command\n");
    }
    if (!status.ok())
    {
        output_.clear();
        static_cast<void>(append("shell error: "));
        static_cast<void>(append(status.message()));
        static_cast<void>(append("\n"));
    }
    return output_.view();
}

Status KernelDebugShell::append(std::string_view text)
{
    return output_.append(text);
}

Status KernelDebugShell::append_unsigned(u64 value)
{
    constexpr u64 powers[] = {
        10'000'000'000'000'000'000ull,
        1'000'000'000'000'000'000ull,
        100'000'000'000'000'000ull,
        10'000'000'000'000'000ull,
        1'000'000'000'000'000ull,
        100'000'000'000'000ull,
        10'000'000'000'000ull,
        1'000'000'000'000ull,
        100'000'000'000ull,
        10'000'000'000ull,
        1'000'000'000ull,
        100'000'000ull,
        10'000'000ull,
        1'000'000ull,
        100'000ull,
        10'000ull,
        1'000ull,
        100ull,
        10ull,
        1ull,
    };
    bool started = false;
    for (const auto power : powers)
    {
        u8 digit = 0;
        while (value >= power)
        {
            value -= power;
            ++digit;
        }
        if (digit != 0 || started || power == 1)
        {
            if (auto status = output_.append(static_cast<char>('0' + digit)); !status.ok())
            {
                return status;
            }
            started = true;
        }
    }
    return Status::success();
}

Status KernelDebugShell::command_help()
{
    return append("help status mem ps drivers fs posix test echo\n");
}

Status KernelDebugShell::command_status()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (auto status = append("arch="); !status.ok())
    {
        return status;
    }
    if (auto status = append(kernel_->arch().name()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" cpus="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->topology().online_count()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" drivers="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->drivers().driver_count()); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_memory()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (auto status = append("page_size="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->memory().frames().page_size()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" free_frames="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->memory().frames().free_frames()); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_processes()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (auto status = append("processes="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->scheduler().process_count()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" current="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->scheduler().current_pid()); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_drivers()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (auto status = append("drivers="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->drivers().driver_count()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" pci="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->pci().device_count()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" usb="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->usb().device_count()); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_filesystem()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    auto stat = kernel_->vfs().stat("/tmp/kernel.log");
    if (!stat)
    {
        return stat.status();
    }
    if (auto status = append("/tmp/kernel.log size="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stat.value().size); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_posix()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (auto status = append("pid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->posix().getpid()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" cwd="); !status.ok())
    {
        return status;
    }
    if (auto status = append(kernel_->posix().getcwd()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" fds="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->posix().open_file_count()); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_tests()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (auto status = append("debug_test_points="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->debug_test_points_run()); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_echo(std::string_view text)
{
    if (auto status = append(text); !status.ok())
    {
        return status;
    }
    return append("\n");
}

} // namespace ok
