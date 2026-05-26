#include "ok/core/shell.hpp"

#include "ok/core/kernel.hpp"
#include "shell_private.hpp"

namespace ok
{

using shell_detail::first_word;
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
        return kernel_->open_task_manager();
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

} // namespace ok
