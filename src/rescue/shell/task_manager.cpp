#include "ok/apps/shell.hpp"

#include "ok/core/kernel.hpp"
#include "shell_private.hpp"

namespace ok
{

using shell_detail::first_word;
using shell_detail::after_first_word;
using shell_detail::trim;

Status KernelDebugShell::command_task_manager(std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }

    const auto mode = first_word(args);
    if (mode == "gui")
    {
        return kernel_->open_task_manager(true);
    }
    if (mode == "close")
    {
        return kernel_->close_task_manager();
    }
    if (!trim(args).empty() && mode != "tui" && mode != "status")
    {
        return Status::invalid_argument("taskman supports: taskman [tui|gui|close]");
    }

    FixedString<4096> snapshot;
    if (auto status = kernel_->task_manager().render_tui(*kernel_, snapshot); !status.ok())
    {
        return status;
    }
    return append(snapshot.view());
}

Status KernelDebugShell::command_top(std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto mode = first_word(args);
    if (mode == "tui" || mode == "status")
    {
        FixedString<4096> snapshot;
        if (auto status = kernel_->task_manager().render_top_tui(*kernel_, snapshot); !status.ok())
        {
            return status;
        }
        return append(snapshot.view());
    }
    if (mode == "gui" || trim(args).empty())
    {
        return kernel_->open_task_manager(true, "top");
    }
    if (mode == "close")
    {
        return kernel_->close_task_manager();
    }
    if (!trim(after_first_word(args)).empty())
    {
        return Status::invalid_argument("top supports: top [gui|tui|close]");
    }
    return Status::invalid_argument("top supports: top [gui|tui|close]");
}

Status KernelDebugShell::tick()
{
    return reconcile_gui_windows();
}

} // namespace ok
