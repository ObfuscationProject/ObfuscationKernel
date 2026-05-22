#include "ok/core/shell.hpp"

#include "ok/core/kernel.hpp"

namespace ok
{

Status KernelDebugShell::command_help()
{
    return append(
        "help true false : clear uname status mem ps drivers fs posix test echo pwd cd ls cat touch mkdir rm stat "
        "whoami id su disk mkfs sfs ext4 net\n");
}

Status KernelDebugShell::command_true()
{
    return Status::success();
}

Status KernelDebugShell::command_false()
{
    return Status{StatusCode::fault};
}

Status KernelDebugShell::command_noop()
{
    return Status::success();
}

Status KernelDebugShell::command_clear()
{
    return append("\f");
}

Status KernelDebugShell::command_uname()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto info = kernel_->posix().uname();
    if (auto status = append(info.sysname.view()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" "); !status.ok())
    {
        return status;
    }
    if (auto status = append(info.nodename.view()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" "); !status.ok())
    {
        return status;
    }
    if (auto status = append(info.release.view()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" "); !status.ok())
    {
        return status;
    }
    if (auto status = append(info.version.view()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" "); !status.ok())
    {
        return status;
    }
    if (auto status = append(info.machine.view()); !status.ok())
    {
        return status;
    }
    return append("\n");
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

} // namespace ok
