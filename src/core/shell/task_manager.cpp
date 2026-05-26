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

Status KernelDebugShell::command_top(std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (!trim(args).empty())
    {
        return Status::invalid_argument("top does not accept arguments");
    }
    return start_realtime_task_manager();
}

Status KernelDebugShell::start_realtime_task_manager()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (top_process_id_ != 0 && foreground_process_id_ == top_process_id_ &&
        kernel_->scheduler().find(top_process_id_) != nullptr)
    {
        return refresh_realtime_task_manager();
    }

    FixedString<4096> snapshot;
    if (auto status = kernel_->task_manager().render_tui(*kernel_, snapshot); !status.ok())
    {
        return status;
    }
    if (auto status = append(snapshot.view()); !status.ok())
    {
        return status;
    }

    const auto process_offset = kernel_->scheduler().process_count() * 0x1000;
    auto process =
        kernel_->create_ui_process("top", 0x7900 + process_offset, 0xf800 + process_offset,
                                   kernel_->posix().user_credentials());
    if (!process)
    {
        return process.status();
    }

    top_process_id_ = process.value();
    if (auto status = start_foreground_process(process.value()); !status.ok())
    {
        top_process_id_ = 0;
        static_cast<void>(kernel_->scheduler().kill_process(process.value()));
        return status;
    }
    return Status::success();
}

Status KernelDebugShell::refresh_realtime_task_manager()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (top_process_id_ == 0 || foreground_process_id_ != top_process_id_)
    {
        return Status::success();
    }
    if (kernel_->scheduler().find(top_process_id_) == nullptr)
    {
        notify_process_exit(top_process_id_);
        return Status::success();
    }

    FixedString<4096> snapshot;
    if (auto status = kernel_->task_manager().render_tui(*kernel_, snapshot); !status.ok())
    {
        return status;
    }
    gui_history_.clear();
    gui_scroll_rows_ = 0;
    if (auto status = append_gui_history("top - Ctrl-C to exit\n"); !status.ok())
    {
        return status;
    }
    if (auto status = append_gui_history(snapshot.view()); !status.ok())
    {
        return status;
    }
    return redraw_gui_terminal();
}

Status KernelDebugShell::tick()
{
    return refresh_realtime_task_manager();
}

} // namespace ok
