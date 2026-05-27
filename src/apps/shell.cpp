#include "ok/apps/shell.hpp"

#include "ok/core/kernel.hpp"
#include "shell/shell_private.hpp"

namespace ok
{
namespace
{

using shell_detail::after_first_word;
using shell_detail::as_bytes;
using shell_detail::first_word;
using shell_detail::trim;

enum class ShellOperator : u8
{
    always,
    and_if,
    or_if,
};

struct Redirection
{
    usize index{0};
    usize width{0};
    bool append{false};
};

template <usize Capacity> Status append_decimal_to(FixedString<Capacity> &out, u64 value)
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
            if (auto status = out.append(static_cast<char>('0' + digit)); !status.ok())
            {
                return status;
            }
            started = true;
        }
    }
    return Status::success();
}

bool is_shell_name_start(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

bool is_shell_name_char(char ch)
{
    return is_shell_name_start(ch) || (ch >= '0' && ch <= '9');
}

std::string_view strip_comment(std::string_view value)
{
    bool single_quote = false;
    bool double_quote = false;
    for (usize i = 0; i < value.size(); ++i)
    {
        const auto ch = value[i];
        if (ch == '\'' && !double_quote)
        {
            single_quote = !single_quote;
        }
        else if (ch == '"' && !single_quote)
        {
            double_quote = !double_quote;
        }
        else if (ch == '#' && !single_quote && !double_quote)
        {
            if (i == 0 || value[i - 1] == ' ' || value[i - 1] == '\t')
            {
                return value.substr(0, i);
            }
        }
    }
    return value;
}

usize find_shell_operator(std::string_view value)
{
    bool single_quote = false;
    bool double_quote = false;
    for (usize i = 0; i < value.size(); ++i)
    {
        const auto ch = value[i];
        if (ch == '\'' && !double_quote)
        {
            single_quote = !single_quote;
            continue;
        }
        if (ch == '"' && !single_quote)
        {
            double_quote = !double_quote;
            continue;
        }
        if (single_quote || double_quote)
        {
            continue;
        }
        if (ch == ';')
        {
            return i;
        }
        if (i + 1 < value.size() && ((ch == '&' && value[i + 1] == '&') || (ch == '|' && value[i + 1] == '|')))
        {
            return i;
        }
    }
    return value.size();
}

usize find_pipe_operator(std::string_view value)
{
    bool single_quote = false;
    bool double_quote = false;
    for (usize i = 0; i < value.size(); ++i)
    {
        const auto ch = value[i];
        if (ch == '\\' && !single_quote)
        {
            ++i;
            continue;
        }
        if (ch == '\'' && !double_quote)
        {
            single_quote = !single_quote;
            continue;
        }
        if (ch == '"' && !single_quote)
        {
            double_quote = !double_quote;
            continue;
        }
        if (!single_quote && !double_quote && ch == '|')
        {
            if (i + 1 < value.size() && value[i + 1] == '|')
            {
                ++i;
                continue;
            }
            return i;
        }
    }
    return value.size();
}

usize find_input_redirection(std::string_view value)
{
    bool single_quote = false;
    bool double_quote = false;
    for (usize i = 0; i < value.size(); ++i)
    {
        const auto ch = value[i];
        if (ch == '\\' && !single_quote)
        {
            ++i;
            continue;
        }
        if (ch == '\'' && !double_quote)
        {
            single_quote = !single_quote;
            continue;
        }
        if (ch == '"' && !single_quote)
        {
            double_quote = !double_quote;
            continue;
        }
        if (!single_quote && !double_quote && ch == '<')
        {
            return i;
        }
    }
    return value.size();
}

Redirection find_output_redirection(std::string_view value)
{
    bool single_quote = false;
    bool double_quote = false;
    for (usize i = 0; i < value.size(); ++i)
    {
        const auto ch = value[i];
        if (ch == '\\' && !single_quote)
        {
            ++i;
            continue;
        }
        if (ch == '\'' && !double_quote)
        {
            single_quote = !single_quote;
            continue;
        }
        if (ch == '"' && !single_quote)
        {
            double_quote = !double_quote;
            continue;
        }
        if (!single_quote && !double_quote && ch == '>')
        {
            return Redirection{.index = i, .width = (i + 1 < value.size() && value[i + 1] == '>') ? 2u : 1u,
                               .append = i + 1 < value.size() && value[i + 1] == '>'};
        }
    }
    return Redirection{.index = value.size()};
}

ShellOperator operator_after(std::string_view value, usize index)
{
    if (index >= value.size() || value[index] == ';')
    {
        return ShellOperator::always;
    }
    return value[index] == '&' ? ShellOperator::and_if : ShellOperator::or_if;
}

std::string_view session_user_for_credentials(const user::UserSpaceManager &users, user::Credentials credentials)
{
    if (credentials.kernel_space)
    {
        return "kernel";
    }
    if (const auto *account = users.users().find_by_uid(credentials.euid); account != nullptr)
    {
        return account->name.view();
    }
    return credentials.euid == user::root_uid ? std::string_view{"root"} : std::string_view{"user"};
}

usize operator_width(std::string_view value, usize index)
{
    if (index >= value.size())
    {
        return 0;
    }
    return value[index] == ';' ? 1 : 2;
}

constexpr gui::Rect shell_gui_bounds{
    .x = 24,
    .y = 22,
    .width = static_cast<u32>(driver::framebuffer_width - 48),
    .height = static_cast<u32>(driver::framebuffer_height - 44),
};
constexpr u32 shell_gui_background = 0xff061018u;
constexpr u32 shell_gui_title_background = 0xff12313du;
constexpr u32 shell_gui_title_border = 0xff44aa88u;
constexpr u32 shell_gui_foreground = 0xffd8f3ffu;
constexpr u32 shell_gui_prompt = 0xfff4d35eu;
constexpr usize shell_gui_content_row = 3;
constexpr usize shell_gui_history_keep = 1800;
constexpr usize shell_gui_scroll_rows_per_notch = 3;

usize decimal_digits(u64 value)
{
    usize digits = 1;
    while (value >= 10)
    {
        value /= 10;
        ++digits;
    }
    return digits;
}

usize visual_line_count(std::string_view text, usize columns)
{
    if (text.empty() || columns == 0)
    {
        return 0;
    }
    usize row = 0;
    usize column = 0;
    for (usize i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++row;
            column = 0;
            continue;
        }
        if (column >= columns)
        {
            ++row;
            column = 0;
        }
        ++column;
    }
    return row + 1;
}

usize visual_line_offset(std::string_view text, usize columns, usize target_row)
{
    if (target_row == 0 || columns == 0)
    {
        return 0;
    }
    usize row = 0;
    usize column = 0;
    for (usize i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            ++row;
            column = 0;
            if (row == target_row)
            {
                return i + 1;
            }
            continue;
        }
        if (column >= columns)
        {
            ++row;
            column = 0;
            if (row == target_row)
            {
                return i;
            }
        }
        ++column;
    }
    return text.size();
}

} // namespace
Status KernelDebugShell::attach(Kernel &kernel)
{
    kernel_ = &kernel;
    static_cast<void>(session_user_name_.assign("kernel"));
    previous_session_user_name_.clear();
    previous_credentials_ = {};
    has_previous_session_ = false;
    path_buffer_.clear();
    output_.clear();
    gui_history_.clear();
    gui_input_line_.clear();
    gui_surface_id_ = 0;
    process_id_ = 0;
    foreground_process_id_ = 0;
    gui_windows_.clear();
    environment_.clear();
    gui_render_count_ = 0;
    gui_scroll_rows_ = 0;
    gui_input_history_count_ = 0;
    gui_input_history_cursor_ = 0;
    last_status_code_ = 0;
    gui_escape_state_ = 0;
    gui_open_ = true;
    if (auto status = set_environment_variable("HOME", "/"); !status.ok())
    {
        return status;
    }
    if (auto status = set_environment_variable("SHELL", "/kernel/apps/oksh"); !status.ok())
    {
        return status;
    }
    if (auto status = set_environment_variable("PATH", "/kernel/apps:/bin:/sbin"); !status.ok())
    {
        return status;
    }
    return sync_posix_credentials_to_session();
}

bool KernelDebugShell::gui_ready()
{
    return gui_open_ && ensure_gui_surface().ok();
}

Result<usize> KernelDebugShell::find_window_by_process(sched::ProcessId pid) const
{
    for (usize i = 0; i < gui_windows_.size(); ++i)
    {
        if (gui_windows_[i].process_id == pid)
        {
            return i;
        }
    }
    return Status::not_found("GUI shell process not found");
}

Result<usize> KernelDebugShell::find_window_by_surface(gui::SurfaceId surface) const
{
    for (usize i = 0; i < gui_windows_.size(); ++i)
    {
        if (gui_windows_[i].surface_id == surface)
        {
            return i;
        }
    }
    return Status::not_found("GUI shell surface not found");
}

bool KernelDebugShell::owns_process(sched::ProcessId pid) const
{
    return find_window_by_process(pid).ok();
}

bool KernelDebugShell::owns_surface(gui::SurfaceId surface) const
{
    return find_window_by_surface(surface).ok();
}

Status KernelDebugShell::close_process_window(sched::ProcessId pid)
{
    auto index = find_window_by_process(pid);
    if (!index)
    {
        return index.status();
    }
    return remove_gui_window(index.value());
}

Status KernelDebugShell::close_surface_window(gui::SurfaceId surface)
{
    auto index = find_window_by_surface(surface);
    if (!index)
    {
        return index.status();
    }
    const auto process = gui_windows_[index.value()].process_id;
    if (kernel_ != nullptr && process != 0 && kernel_->scheduler().find(process) != nullptr)
    {
        return kernel_->kill_process(process);
    }
    return remove_gui_window(index.value());
}

Status KernelDebugShell::handle_surface_changed(gui::SurfaceId surface)
{
    auto index = find_window_by_surface(surface);
    if (!index)
    {
        return index.status();
    }
    if (auto status = select_gui_window(index.value()); !status.ok())
    {
        return status;
    }
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    auto info = kernel_->gui().compositor().surface_info(surface);
    if (!info)
    {
        return info.status();
    }
    if (info.value().window_state == gui::WindowState::minimized)
    {
        return kernel_->gui().compositor().present();
    }
    return redraw_gui_terminal();
}

Status KernelDebugShell::record_gui_window()
{
    if (gui_surface_id_ == 0 || process_id_ == 0)
    {
        return Status::success();
    }
    for (auto &window : gui_windows_)
    {
        if (window.process_id == process_id_)
        {
            window.surface_id = gui_surface_id_;
            window.foreground_process_id = foreground_process_id_;
            if (auto status = window.session_user_name.assign(session_user_name_.view()); !status.ok())
            {
                return status;
            }
            if (auto status = window.previous_session_user_name.assign(previous_session_user_name_.view());
                !status.ok())
            {
                return status;
            }
            window.previous_credentials = previous_credentials_;
            window.has_previous_session = has_previous_session_;
            if (auto status = window.history.assign(gui_history_.view()); !status.ok())
            {
                return status;
            }
            if (auto status = window.input_line.assign(gui_input_line_.view()); !status.ok())
            {
                return status;
            }
            window.input_history = gui_input_history_;
            window.scroll_rows = gui_scroll_rows_;
            window.input_history_count = gui_input_history_count_;
            window.input_history_cursor = gui_input_history_cursor_;
            window.escape_state = gui_escape_state_;
            return Status::success();
        }
    }
    GuiWindow window{.surface_id = gui_surface_id_, .process_id = process_id_};
    window.foreground_process_id = foreground_process_id_;
    if (auto status = window.session_user_name.assign(session_user_name_.view()); !status.ok())
    {
        return status;
    }
    if (auto status = window.previous_session_user_name.assign(previous_session_user_name_.view()); !status.ok())
    {
        return status;
    }
    window.previous_credentials = previous_credentials_;
    window.has_previous_session = has_previous_session_;
    if (auto status = window.history.assign(gui_history_.view()); !status.ok())
    {
        return status;
    }
    if (auto status = window.input_line.assign(gui_input_line_.view()); !status.ok())
    {
        return status;
    }
    window.input_history = gui_input_history_;
    window.scroll_rows = gui_scroll_rows_;
    window.input_history_count = gui_input_history_count_;
    window.input_history_cursor = gui_input_history_cursor_;
    window.escape_state = gui_escape_state_;
    return gui_windows_.push_back(window);
}

Status KernelDebugShell::save_active_gui_window_state()
{
    if (process_id_ == 0)
    {
        return Status::success();
    }
    auto index = find_window_by_process(process_id_);
    if (!index)
    {
        return Status::success();
    }
    auto &window = gui_windows_[index.value()];
    window.surface_id = gui_surface_id_;
    window.foreground_process_id = foreground_process_id_;
    if (auto status = window.session_user_name.assign(session_user_name_.view()); !status.ok())
    {
        return status;
    }
    if (auto status = window.previous_session_user_name.assign(previous_session_user_name_.view()); !status.ok())
    {
        return status;
    }
    window.previous_credentials = previous_credentials_;
    window.has_previous_session = has_previous_session_;
    if (auto status = window.history.assign(gui_history_.view()); !status.ok())
    {
        return status;
    }
    if (auto status = window.input_line.assign(gui_input_line_.view()); !status.ok())
    {
        return status;
    }
    window.input_history = gui_input_history_;
    window.scroll_rows = gui_scroll_rows_;
    window.input_history_count = gui_input_history_count_;
    window.input_history_cursor = gui_input_history_cursor_;
    window.escape_state = gui_escape_state_;
    return Status::success();
}

Status KernelDebugShell::load_gui_window_state(usize index)
{
    if (index >= gui_windows_.size())
    {
        return Status::invalid_argument("GUI shell window index out of range");
    }
    const auto &window = gui_windows_[index];
    gui_open_ = true;
    gui_surface_id_ = window.surface_id;
    process_id_ = window.process_id;
    foreground_process_id_ = window.foreground_process_id;
    if (auto status = session_user_name_.assign(window.session_user_name.view()); !status.ok())
    {
        return status;
    }
    if (auto status = previous_session_user_name_.assign(window.previous_session_user_name.view()); !status.ok())
    {
        return status;
    }
    previous_credentials_ = window.previous_credentials;
    has_previous_session_ = window.has_previous_session;
    if (auto status = gui_history_.assign(window.history.view()); !status.ok())
    {
        return status;
    }
    if (auto status = gui_input_line_.assign(window.input_line.view()); !status.ok())
    {
        return status;
    }
    gui_input_history_ = window.input_history;
    gui_scroll_rows_ = window.scroll_rows;
    gui_input_history_count_ = window.input_history_count;
    gui_input_history_cursor_ = window.input_history_cursor;
    gui_escape_state_ = window.escape_state;
    if (auto status = sync_posix_credentials_to_session(); !status.ok())
    {
        return status;
    }
    return refresh_process_credentials();
}

Status KernelDebugShell::reset_gui_session_state()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto credentials = kernel_->posix().user_credentials();
    if (auto status = session_user_name_.assign(session_user_for_credentials(kernel_->user_space(), credentials));
        !status.ok())
    {
        return status;
    }
    previous_session_user_name_.clear();
    previous_credentials_ = {};
    has_previous_session_ = false;
    foreground_process_id_ = 0;
    gui_history_.clear();
    gui_input_line_.clear();
    gui_scroll_rows_ = 0;
    gui_escape_state_ = 0;
    gui_input_history_count_ = 0;
    gui_input_history_cursor_ = 0;
    return Status::success();
}

Status KernelDebugShell::select_gui_window(usize index)
{
    if (index >= gui_windows_.size())
    {
        return Status::invalid_argument("GUI shell window index out of range");
    }
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }

    const auto window = gui_windows_[index];
    auto &compositor = kernel_->gui().compositor();
    if (window.surface_id == 0 || window.process_id == 0 || !compositor.surface_info(window.surface_id) ||
        kernel_->scheduler().find(window.process_id) == nullptr)
    {
        return Status::not_found("GUI shell window not found");
    }

    if (auto status = save_active_gui_window_state(); !status.ok())
    {
        return status;
    }
    return load_gui_window_state(index);
}

Status KernelDebugShell::activate_gui_window(usize index)
{
    if (auto status = select_gui_window(index); !status.ok())
    {
        return status;
    }

    const auto window = gui_windows_[index];
    auto &compositor = kernel_->gui().compositor();
    auto info = compositor.surface_info(window.surface_id);
    if (!info)
    {
        return info.status();
    }
    if (info.value().window_state == gui::WindowState::minimized)
    {
        if (auto status = compositor.restore_surface(window.surface_id); !status.ok())
        {
            return status;
        }
    }
    else if (auto status = compositor.raise_surface(window.surface_id); !status.ok())
    {
        return status;
    }
    return redraw_gui_terminal();
}

Status KernelDebugShell::remove_gui_window(usize index)
{
    if (index >= gui_windows_.size())
    {
        return Status::invalid_argument("GUI shell window index out of range");
    }
    const auto window = gui_windows_[index];
    if (kernel_ != nullptr && window.surface_id != 0)
    {
        auto &compositor = kernel_->gui().compositor();
        if (compositor.state() == gui::GuiState::running && compositor.surface_info(window.surface_id))
        {
            if (auto status = compositor.destroy_surface(window.surface_id); !status.ok())
            {
                return status;
            }
        }
    }
    if (window.process_id == process_id_)
    {
        gui_surface_id_ = 0;
        process_id_ = 0;
        foreground_process_id_ = 0;
        gui_open_ = false;
    }
    if (auto status = gui_windows_.erase_at(index); !status.ok())
    {
        return status;
    }
    if (kernel_ != nullptr && window.surface_id != 0)
    {
        auto &compositor = kernel_->gui().compositor();
        if (compositor.state() == gui::GuiState::running)
        {
            static_cast<void>(compositor.present());
        }
    }
    return save_active_gui_window_state();
}

Status KernelDebugShell::ensure_gui_process()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (auto status = sync_posix_credentials_to_session(); !status.ok())
    {
        return status;
    }
    if (process_id_ != 0)
    {
        if (auto *process = kernel_->scheduler().find(process_id_);
            process != nullptr && process->state() != sched::ProcessState::exited)
        {
            return refresh_process_credentials();
        }
        if (auto index = find_window_by_process(process_id_))
        {
            static_cast<void>(gui_windows_.erase_at(index.value()));
        }
        process_id_ = 0;
        foreground_process_id_ = 0;
    }

    const auto process_offset = kernel_->scheduler().process_count() * 0x1000;
    auto process = kernel_->create_ui_process("oksh", 0x6000 + process_offset, 0xd000 + process_offset,
                                             kernel_->posix().user_credentials());
    if (!process)
    {
        return process.status();
    }
    process_id_ = process.value();
    return refresh_process_credentials();
}

Status KernelDebugShell::refresh_process_credentials()
{
    if (kernel_ == nullptr || process_id_ == 0)
    {
        return Status::success();
    }
    return kernel_->scheduler().set_credentials(process_id_, kernel_->posix().user_credentials());
}

Status KernelDebugShell::sync_posix_credentials_to_session()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    auto target = kernel_->user_space().credentials_for(session_user_name_.view());
    if (!target)
    {
        return target.status();
    }
    const auto active = kernel_->posix().user_credentials();
    const auto desired = target.value();
    if (active.uid == desired.uid && active.gid == desired.gid && active.euid == desired.euid &&
        active.egid == desired.egid && active.kernel_space == desired.kernel_space)
    {
        return Status::success();
    }
    return kernel_->posix().set_credentials(desired);
}

bool KernelDebugShell::foreground_process_running()
{
    if (foreground_process_id_ == 0 || kernel_ == nullptr)
    {
        return false;
    }
    auto *process = kernel_->scheduler().find(foreground_process_id_);
    if (process == nullptr || process->state() == sched::ProcessState::exited)
    {
        notify_process_exit(foreground_process_id_);
        return false;
    }
    return true;
}

Status KernelDebugShell::start_foreground_process(sched::ProcessId pid)
{
    if (pid == 0 || kernel_ == nullptr || kernel_->scheduler().find(pid) == nullptr)
    {
        return Status::not_found("foreground process not found");
    }
    if (auto status = ensure_gui_process(); !status.ok())
    {
        return status;
    }
    foreground_process_id_ = pid;
    if (process_id_ != 0)
    {
        if (auto *process = kernel_->scheduler().find(process_id_); process != nullptr)
        {
            process->set_state(sched::ProcessState::blocked);
            for (auto &thread : process->threads())
            {
                thread.state = sched::ProcessState::blocked;
            }
        }
    }
    return save_active_gui_window_state();
}

Status KernelDebugShell::interrupt_foreground_process()
{
    if (foreground_process_id_ == 0)
    {
        gui_input_line_.clear();
        static_cast<void>(append_gui_history("^C\n"));
        return redraw_gui_terminal();
    }
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto pid = foreground_process_id_;
    static_cast<void>(append_gui_history("^C\n"));
    return kernel_->kill_process(pid);
}

void KernelDebugShell::notify_process_exit(sched::ProcessId pid)
{
    if (pid == 0)
    {
        return;
    }
    for (auto &window : gui_windows_)
    {
        if (window.foreground_process_id == pid)
        {
            window.foreground_process_id = 0;
            if (kernel_ != nullptr && window.process_id != 0)
            {
                static_cast<void>(kernel_->scheduler().set_runnable(window.process_id));
            }
        }
    }
    if (foreground_process_id_ == pid)
    {
        foreground_process_id_ = 0;
        if (kernel_ != nullptr && process_id_ != 0)
        {
            static_cast<void>(kernel_->scheduler().set_runnable(process_id_));
        }
        static_cast<void>(append_gui_history("process exited pid="));
        static_cast<void>(append_gui_history_unsigned(pid));
        static_cast<void>(append_gui_history("\n"));
        static_cast<void>(redraw_gui_terminal());
    }
    if (process_id_ == pid)
    {
        process_id_ = 0;
        foreground_process_id_ = 0;
    }
    if (auto index = find_window_by_process(pid))
    {
        static_cast<void>(gui_windows_.erase_at(index.value()));
    }
}

Status KernelDebugShell::show_gui()
{
    if (auto status = reconcile_gui_windows(); !status.ok())
    {
        return status;
    }
    if (auto status = save_active_gui_window_state(); !status.ok())
    {
        return status;
    }
    gui_open_ = true;
    gui_surface_id_ = 0;
    process_id_ = 0;
    if (auto status = reset_gui_session_state(); !status.ok())
    {
        return status;
    }
    return redraw_gui_terminal();
}

Status KernelDebugShell::show_or_focus_gui()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    auto &compositor = kernel_->gui().compositor();
    if (compositor.state() == gui::GuiState::running)
    {
        for (usize i = 0; i < gui_windows_.size(); ++i)
        {
            const auto window = gui_windows_[i];
            if (window.surface_id != 0 && compositor.surface_info(window.surface_id) && window.process_id != 0 &&
                kernel_->scheduler().find(window.process_id) != nullptr)
            {
                return activate_gui_window(i);
            }
        }
    }
    return show_gui();
}

Status KernelDebugShell::close_gui()
{
    gui_open_ = false;
    gui_history_.clear();
    gui_input_line_.clear();
    gui_scroll_rows_ = 0;
    gui_escape_state_ = 0;
    gui_input_history_count_ = 0;
    gui_input_history_cursor_ = 0;
    gui_input_history_count_ = 0;
    gui_input_history_cursor_ = 0;
    const auto process = process_id_;
    process_id_ = 0;
    foreground_process_id_ = 0;
    if (kernel_ != nullptr && process != 0)
    {
        static_cast<void>(kernel_->scheduler().kill_process(process));
    }
    if (auto index = find_window_by_process(process))
    {
        static_cast<void>(remove_gui_window(index.value()));
    }
    if (kernel_ == nullptr || gui_surface_id_ == 0)
    {
        gui_surface_id_ = 0;
        return Status::success();
    }
    const auto id = gui_surface_id_;
    gui_surface_id_ = 0;
    auto &compositor = kernel_->gui().compositor();
    if (compositor.surface_info(id))
    {
        if (auto status = compositor.destroy_surface(id); !status.ok())
        {
            return status;
        }
        return compositor.present();
    }
    return Status::success();
}

Status KernelDebugShell::close_all_gui()
{
    gui_open_ = false;
    gui_history_.clear();
    gui_input_line_.clear();
    gui_scroll_rows_ = 0;
    gui_escape_state_ = 0;

    if (kernel_ != nullptr)
    {
        auto &compositor = kernel_->gui().compositor();
        for (usize i = gui_windows_.size(); i != 0; --i)
        {
            const auto window = gui_windows_[i - 1];
            if (window.process_id != 0 && kernel_->scheduler().find(window.process_id) != nullptr)
            {
                static_cast<void>(kernel_->scheduler().kill_process(window.process_id));
            }
            if (window.surface_id != 0 && compositor.state() == gui::GuiState::running &&
                compositor.surface_info(window.surface_id))
            {
                if (auto status = compositor.destroy_surface(window.surface_id); !status.ok())
                {
                    return status;
                }
            }
            static_cast<void>(gui_windows_.erase_at(i - 1));
        }
        if (compositor.state() == gui::GuiState::running)
        {
            static_cast<void>(compositor.present());
        }
    }
    else
    {
        gui_windows_.clear();
    }

    gui_surface_id_ = 0;
    process_id_ = 0;
    foreground_process_id_ = 0;
    return Status::success();
}

void KernelDebugShell::mark_gui_closed()
{
    gui_open_ = false;
    gui_surface_id_ = 0;
    process_id_ = 0;
    foreground_process_id_ = 0;
    gui_history_.clear();
    gui_input_line_.clear();
    gui_scroll_rows_ = 0;
    gui_escape_state_ = 0;
    previous_session_user_name_.clear();
    previous_credentials_ = {};
    has_previous_session_ = false;
    static_cast<void>(session_user_name_.assign("kernel"));
}

Status KernelDebugShell::reconcile_gui_windows()
{
    if (kernel_ == nullptr || kernel_->gui().compositor().state() != gui::GuiState::running)
    {
        return Status::success();
    }
    auto &compositor = kernel_->gui().compositor();
    for (usize i = 0; i < gui_windows_.size();)
    {
        const auto window = gui_windows_[i];
        const bool surface_alive = window.surface_id != 0 && compositor.surface_info(window.surface_id);
        const bool process_alive = window.process_id != 0 && kernel_->scheduler().find(window.process_id) != nullptr;
        if (surface_alive && process_alive)
        {
            ++i;
            continue;
        }
        if (process_alive)
        {
            static_cast<void>(kernel_->scheduler().kill_process(window.process_id));
        }
        if (window.process_id == process_id_)
        {
            mark_gui_closed();
        }
        static_cast<void>(gui_windows_.erase_at(i));
    }
    return Status::success();
}

Status KernelDebugShell::set_gui_input(std::string_view line)
{
    if (!gui_open_)
    {
        return Status::success();
    }
    gui_scroll_rows_ = 0;
    gui_input_line_.clear();
    if (auto status = gui_input_line_.append(line); !status.ok())
    {
        return status;
    }
    return redraw_gui_terminal();
}

Status KernelDebugShell::remember_gui_input_line()
{
    if (gui_input_line_.empty())
    {
        gui_input_history_cursor_ = gui_input_history_count_;
        return Status::success();
    }
    if (gui_input_history_count_ != 0 &&
        gui_input_history_[gui_input_history_count_ - 1].view() == gui_input_line_.view())
    {
        gui_input_history_cursor_ = gui_input_history_count_;
        return Status::success();
    }
    if (gui_input_history_count_ == gui_input_history_.size())
    {
        for (usize i = 1; i < gui_input_history_.size(); ++i)
        {
            gui_input_history_[i - 1] = gui_input_history_[i];
        }
        --gui_input_history_count_;
    }
    if (auto status = gui_input_history_[gui_input_history_count_].assign(gui_input_line_.view()); !status.ok())
    {
        return status;
    }
    ++gui_input_history_count_;
    gui_input_history_cursor_ = gui_input_history_count_;
    return Status::success();
}

Status KernelDebugShell::recall_gui_history_previous()
{
    if (gui_input_history_count_ == 0 || gui_input_history_cursor_ == 0)
    {
        return Status::success();
    }
    --gui_input_history_cursor_;
    return set_gui_input(gui_input_history_[gui_input_history_cursor_].view());
}

Status KernelDebugShell::recall_gui_history_next()
{
    if (gui_input_history_cursor_ >= gui_input_history_count_)
    {
        return Status::success();
    }
    ++gui_input_history_cursor_;
    if (gui_input_history_cursor_ == gui_input_history_count_)
    {
        return set_gui_input("");
    }
    return set_gui_input(gui_input_history_[gui_input_history_cursor_].view());
}

Status KernelDebugShell::handle_key(gui::SurfaceId surface, int key)
{
    auto index = find_window_by_surface(surface);
    if (!index)
    {
        return index.status();
    }
    if (surface != gui_surface_id_)
    {
        if (auto status = activate_gui_window(index.value()); !status.ok())
        {
            return status;
        }
    }
    return handle_key(key);
}

Status KernelDebugShell::handle_key(int key)
{
    if (!gui_open_)
    {
        return Status::success();
    }
    if (key == 0x03)
    {
        gui_escape_state_ = 0;
        return interrupt_foreground_process();
    }
    if (foreground_process_running())
    {
        return Status::success();
    }
    if (key == 0x1b)
    {
        gui_escape_state_ = 1;
        return Status::success();
    }
    if (gui_escape_state_ == 1)
    {
        gui_escape_state_ = key == '[' ? 2 : 0;
        return Status::success();
    }
    if (gui_escape_state_ == 2)
    {
        gui_escape_state_ = 0;
        if (key == 'A')
        {
            return recall_gui_history_previous();
        }
        if (key == 'B')
        {
            return recall_gui_history_next();
        }
        return Status::success();
    }
    if (key == '\r' || key == '\n')
    {
        FixedString<128> command;
        if (auto status = command.assign(gui_input_line_.view()); !status.ok())
        {
            return status;
        }
        if (auto status = remember_gui_input_line(); !status.ok())
        {
            return status;
        }
        gui_input_line_.clear();
        auto result = execute(command.view());
        return result ? Status::success() : result.status();
    }
    if (key == '\b' || key == 0x7f)
    {
        gui_input_line_.pop_back();
        return redraw_gui_terminal();
    }
    if (key == 0x15)
    {
        gui_input_line_.clear();
        return redraw_gui_terminal();
    }
    if (key >= 0x20 && key <= 0x7e)
    {
        if (auto status = gui_input_line_.append(static_cast<char>(key)); !status.ok())
        {
            return status;
        }
        gui_input_history_cursor_ = gui_input_history_count_;
        return redraw_gui_terminal();
    }
    return Status::success();
}

Status KernelDebugShell::scroll_gui_history(i32 rows)
{
    if (!gui_open_)
    {
        return Status::success();
    }
    if (rows == 0)
    {
        return Status::success();
    }
    const auto magnitude =
        rows > 0 ? static_cast<usize>(rows) : static_cast<usize>(-(rows + 1)) + static_cast<usize>(1);
    const auto amount = magnitude * shell_gui_scroll_rows_per_notch;
    if (rows > 0)
    {
        const auto room = shell_gui_history_keep - gui_scroll_rows_;
        gui_scroll_rows_ += amount > room ? room : amount;
    }
    else if (amount >= gui_scroll_rows_)
    {
        gui_scroll_rows_ = 0;
    }
    else
    {
        gui_scroll_rows_ -= amount;
    }
    return redraw_gui_terminal();
}

Result<std::string_view> KernelDebugShell::execute(std::string_view line)
{
    output_.clear();
    auto remaining = trim(strip_comment(line));
    if (remaining.empty())
    {
        return output_.view();
    }
    if (foreground_process_running())
    {
        return Status::would_block("foreground process is running");
    }
    if (auto status = sync_posix_credentials_to_session(); !status.ok())
    {
        return status;
    }

    Status last_status = Status::success();
    auto gate = ShellOperator::always;
    while (!remaining.empty())
    {
        const auto split = find_shell_operator(remaining);
        const auto command_line = trim(remaining.substr(0, split));
        const auto next_gate = operator_after(remaining, split);
        const auto next_offset = split + operator_width(remaining, split);

        const bool should_run = gate == ShellOperator::always || (gate == ShellOperator::and_if && last_status.ok()) ||
                                (gate == ShellOperator::or_if && !last_status.ok());
        if (should_run && !command_line.empty())
        {
            last_status = dispatch_command(command_line);
            last_status_code_ = last_status.ok() ? 0u : 1u;
            if (!last_status.ok() && !last_status.message().empty())
            {
                static_cast<void>(append("shell error: "));
                static_cast<void>(append(last_status.message()));
                static_cast<void>(append("\n"));
            }
        }

        if (next_offset >= remaining.size())
        {
            break;
        }
        remaining.remove_prefix(next_offset);
        remaining = trim(remaining);
        gate = next_gate;
    }
    static_cast<void>(render_to_gui(line, output_.view()));
    if (auto status = save_active_gui_window_state(); !status.ok())
    {
        return status;
    }
    return output_.view();
}

Status KernelDebugShell::dispatch_command(std::string_view command_line)
{
    return dispatch_command_with_input(command_line, {}, false);
}

Status KernelDebugShell::dispatch_command_with_input(std::string_view command_line, std::string_view input,
                                                     bool has_input)
{
    const auto pipe = find_pipe_operator(command_line);
    if (pipe < command_line.size())
    {
        const auto producer = trim(command_line.substr(0, pipe));
        const auto consumer = trim(command_line.substr(pipe + 1));
        if (producer.empty() || consumer.empty())
        {
            return Status::invalid_argument("pipe requires commands on both sides");
        }

        const auto output_start = output_.size();
        const auto producer_status = dispatch_command_with_input(producer, input, has_input);
        FixedString<4096> piped;
        if (producer_status.ok())
        {
            if (auto status = piped.assign(output_.view().substr(output_start)); !status.ok())
            {
                return status;
            }
        }
        while (output_.size() > output_start)
        {
            output_.pop_back();
        }
        if (!producer_status.ok())
        {
            return producer_status;
        }
        return dispatch_command_with_input(consumer, piped.view(), true);
    }

    const auto redirect = find_output_redirection(command_line);
    if (redirect.index < command_line.size())
    {
        const auto producer = trim(command_line.substr(0, redirect.index));
        const auto target = trim(command_line.substr(redirect.index + redirect.width));
        if (producer.empty() || target.empty())
        {
            return Status::invalid_argument("output redirection requires a command and path");
        }
        if (find_output_redirection(target).index < target.size())
        {
            return Status::invalid_argument("multiple output redirections are not supported");
        }

        const auto output_start = output_.size();
        const auto producer_status = dispatch_command_with_input(producer, input, has_input);
        FixedString<4096> redirected;
        if (producer_status.ok())
        {
            if (auto status = redirected.assign(output_.view().substr(output_start)); !status.ok())
            {
                return status;
            }
        }
        while (output_.size() > output_start)
        {
            output_.pop_back();
        }
        if (!producer_status.ok())
        {
            return producer_status;
        }

        FixedString<512> expanded_target;
        if (auto status = expand_command_line(target, expanded_target); !status.ok())
        {
            return status;
        }
        auto resolved = resolve_path(expanded_target.view());
        if (!resolved)
        {
            return resolved.status();
        }
        const auto flags = posix::o_CREAT | posix::o_WRONLY | (redirect.append ? posix::o_APPEND : posix::o_TRUNC);
        auto fd = kernel_->posix().open(resolved.value(), flags);
        if (!fd)
        {
            return fd.status();
        }
        auto written = kernel_->posix().write(fd.value(), as_bytes(redirected.view()));
        if (!written)
        {
            static_cast<void>(kernel_->posix().close(fd.value()));
            return written.status();
        }
        return kernel_->posix().close(fd.value());
    }

    const auto input_redirect = find_input_redirection(command_line);
    if (input_redirect < command_line.size())
    {
        const auto consumer = trim(command_line.substr(0, input_redirect));
        const auto target = trim(command_line.substr(input_redirect + 1));
        if (consumer.empty() || target.empty())
        {
            return Status::invalid_argument("input redirection requires a command and path");
        }
        if (find_input_redirection(target) < target.size())
        {
            return Status::invalid_argument("multiple input redirections are not supported");
        }

        FixedString<512> expanded_target;
        if (auto status = expand_command_line(target, expanded_target); !status.ok())
        {
            return status;
        }
        auto resolved = resolve_path(expanded_target.view());
        if (!resolved)
        {
            return resolved.status();
        }
        auto fd = kernel_->posix().open(resolved.value(), posix::o_RDONLY);
        if (!fd)
        {
            return fd.status();
        }
        std::array<std::byte, fs::max_file_data> buffer{};
        auto read = kernel_->posix().read(fd.value(), buffer);
        static_cast<void>(kernel_->posix().close(fd.value()));
        if (!read)
        {
            return read.status();
        }
        FixedString<4096> redirected_input;
        for (usize i = 0; i < read.value(); ++i)
        {
            if (auto status = redirected_input.append(static_cast<char>(buffer[i])); !status.ok())
            {
                return status;
            }
        }
        return dispatch_command_with_input(consumer, redirected_input.view(), true);
    }

    FixedString<512> expanded;
    if (auto status = expand_command_line(command_line, expanded); !status.ok())
    {
        return status;
    }
    command_line = expanded.view();

    const auto command = first_word(command_line);
    const auto args = after_first_word(command_line);
    if (command.empty())
    {
        return Status::success();
    }
    if (has_input)
    {
        auto filter_status = command_filter(command, args, input, true);
        if (filter_status.ok() || filter_status.code() != StatusCode::not_found)
        {
            return filter_status;
        }
    }
    if (command == "help")
    {
        return command_help();
    }
    if (command == "true")
    {
        return command_true();
    }
    if (command == "false")
    {
        return command_false();
    }
    if (command == ":")
    {
        return command_noop();
    }
    if (command == "clear")
    {
        return command_clear();
    }
    if (command == "uname")
    {
        return command_uname();
    }
    else if (command == "status")
    {
        return command_status();
    }
    else if (command == "mem")
    {
        return command_memory();
    }
    else if (command == "ps")
    {
        return command_processes(args);
    }
    else if (command == "drivers")
    {
        return command_drivers();
    }
    else if (command == "fs")
    {
        return command_filesystem();
    }
    else if (command == "posix")
    {
        return command_posix();
    }
    else if (command == "test")
    {
        return command_tests();
    }
    else if (command == "echo")
    {
        return command_echo(args);
    }
    else if (command == "pwd")
    {
        return command_pwd();
    }
    else if (command == "cd")
    {
        return command_cd(args);
    }
    else if (command == "ls")
    {
        return command_ls(args);
    }
    else if (command == "cat")
    {
        return command_cat(args);
    }
    else if (command == "cp")
    {
        return command_cp(args);
    }
    else if (command == "mv")
    {
        return command_mv(args);
    }
    else if (command == "touch")
    {
        return command_touch(args);
    }
    else if (command == "mkdir")
    {
        return command_mkdir(args);
    }
    else if (command == "rm")
    {
        return command_rm(args);
    }
    else if (command == "rmdir")
    {
        return command_rmdir(args);
    }
    else if (command == "stat")
    {
        return command_stat(args);
    }
    else if (command == "chmod")
    {
        return command_chmod(args);
    }
    else if (command == "chown")
    {
        return command_chown(args);
    }
    else if (command == "users")
    {
        return command_users();
    }
    else if (command == "kill")
    {
        return command_kill(args);
    }
    else if (command == "shutdown" || command == "poweroff")
    {
        return command_power(SystemPowerAction::poweroff, args);
    }
    else if (command == "halt")
    {
        return command_power(SystemPowerAction::halt, args);
    }
    else if (command == "reboot")
    {
        return command_power(SystemPowerAction::reboot, args);
    }
    else if (command == "whoami")
    {
        return command_whoami();
    }
    else if (command == "id")
    {
        return command_id();
    }
    else if (command == "su")
    {
        return command_su(args);
    }
    else if (command == "exit")
    {
        return command_exit(args);
    }
    else if (command == "disk")
    {
        return command_disk();
    }
    else if (command == "mkfs")
    {
        return command_mkfs(args);
    }
    else if (command == "sfs")
    {
        return command_simplefs(args);
    }
    else if (command == "ext4")
    {
        return command_ext4(args);
    }
    else if (command == "net")
    {
        return command_net(args);
    }
    else if (command == "fm" || command == "fileman")
    {
        return command_file_manager(args);
    }
    else if (command == "top")
    {
        return command_top(args);
    }
    else if (command == "taskman")
    {
        return command_task_manager(args);
    }
    else if (command == "history")
    {
        return command_history();
    }
    else if (command == "env")
    {
        return command_env();
    }
    else if (command == "export")
    {
        return command_export(args);
    }
    else if (command == "unset")
    {
        return command_unset(args);
    }
    else if (command == "type")
    {
        return command_type(args);
    }
    else if (command == "which")
    {
        return command_which(args);
    }
    else if (command == "grep" || command == "wc" || command == "head" || command == "tail")
    {
        return command_filter(command, args, input, false);
    }
    else
    {
        return Status::not_found("command not found");
    }
}

Status KernelDebugShell::expand_command_line(std::string_view command_line, FixedString<512> &out)
{
    out.clear();
    bool single_quote = false;
    bool double_quote = false;
    for (usize i = 0; i < command_line.size(); ++i)
    {
        const auto ch = command_line[i];
        if (ch == '\'' && !double_quote)
        {
            single_quote = !single_quote;
            continue;
        }
        if (ch == '"' && !single_quote)
        {
            double_quote = !double_quote;
            continue;
        }
        if (ch == '\\' && !single_quote)
        {
            if (i + 1 < command_line.size())
            {
                ++i;
                if (auto status = out.append(command_line[i]); !status.ok())
                {
                    return status;
                }
            }
            continue;
        }
        if (ch == '$' && !single_quote)
        {
            if (i + 1 < command_line.size() && command_line[i + 1] == '?')
            {
                if (auto status = append_variable_value("?", out); !status.ok())
                {
                    return status;
                }
                ++i;
                continue;
            }
            if (i + 1 < command_line.size() && is_shell_name_start(command_line[i + 1]))
            {
                const auto start = i + 1;
                usize end = start;
                while (end < command_line.size() && is_shell_name_char(command_line[end]))
                {
                    ++end;
                }
                if (auto status = append_variable_value(command_line.substr(start, end - start), out); !status.ok())
                {
                    return status;
                }
                i = end - 1;
                continue;
            }
        }
        if (auto status = out.append(ch); !status.ok())
        {
            return status;
        }
    }
    if (single_quote || double_quote)
    {
        return Status::invalid_argument("unterminated quote");
    }
    return Status::success();
}

Status KernelDebugShell::append_variable_value(std::string_view name, FixedString<512> &out)
{
    if (name == "?")
    {
        return append_decimal_to(out, last_status_code_);
    }
    if (name == "PWD")
    {
        return kernel_ == nullptr ? Status::not_initialized("shell has no kernel") : out.append(kernel_->posix().getcwd());
    }
    if (name == "USER")
    {
        return out.append(session_user_name_.view());
    }
    for (const auto &entry : environment_)
    {
        if (entry.name.view() == name)
        {
            return out.append(entry.value.view());
        }
    }
    return Status::success();
}

Status KernelDebugShell::set_environment_variable(std::string_view name, std::string_view value)
{
    if (name.empty() || !is_shell_name_start(name.front()))
    {
        return Status::invalid_argument("environment variable name is invalid");
    }
    for (const auto ch : name)
    {
        if (!is_shell_name_char(ch))
        {
            return Status::invalid_argument("environment variable name is invalid");
        }
    }
    for (auto &entry : environment_)
    {
        if (entry.name.view() == name)
        {
            return entry.value.assign(value);
        }
    }
    EnvironmentVariable entry;
    if (auto status = entry.name.assign(name); !status.ok())
    {
        return status;
    }
    if (auto status = entry.value.assign(value); !status.ok())
    {
        return status;
    }
    return environment_.push_back(entry);
}

Status KernelDebugShell::command_filter(std::string_view command, std::string_view args, std::string_view input,
                                        bool has_input)
{
    args = trim(args);
    if (command == "cat")
    {
        if (!has_input)
        {
            return Status::not_found("filter not active");
        }
        if (!args.empty() && args != "-")
        {
            return Status::invalid_argument("cat in a pipeline accepts only stdin");
        }
        return append(input);
    }
    if (command == "grep")
    {
        if (!has_input)
        {
            return Status::invalid_argument("grep requires pipeline or input redirection");
        }
        const auto pattern = first_word(args);
        if (pattern.empty())
        {
            return Status::invalid_argument("grep requires a pattern");
        }
        usize line_start = 0;
        for (usize i = 0; i <= input.size(); ++i)
        {
            if (i != input.size() && input[i] != '\n')
            {
                continue;
            }
            const auto line = input.substr(line_start, i - line_start);
            bool found = pattern.empty();
            if (!found && pattern.size() <= line.size())
            {
                for (usize cursor = 0; cursor + pattern.size() <= line.size(); ++cursor)
                {
                    if (line.substr(cursor, pattern.size()) == pattern)
                    {
                        found = true;
                        break;
                    }
                }
            }
            if (found)
            {
                if (auto status = append(line); !status.ok())
                {
                    return status;
                }
                if (auto status = append("\n"); !status.ok())
                {
                    return status;
                }
            }
            line_start = i + 1;
        }
        return Status::success();
    }
    if (command == "wc")
    {
        if (!has_input)
        {
            return Status::invalid_argument("wc requires pipeline or input redirection");
        }
        bool only_lines = args == "-l";
        bool only_bytes = args == "-c";
        bool only_words = args == "-w";
        if (!args.empty() && !only_lines && !only_bytes && !only_words)
        {
            return Status::unsupported("wc supports -l, -w, or -c");
        }
        usize lines = 0;
        usize words = 0;
        bool in_word = false;
        for (const auto ch : input)
        {
            if (ch == '\n')
            {
                ++lines;
            }
            const bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
            if (space)
            {
                in_word = false;
            }
            else if (!in_word)
            {
                in_word = true;
                ++words;
            }
        }
        if (only_lines)
        {
            if (auto status = append_unsigned(lines); !status.ok())
            {
                return status;
            }
            return append("\n");
        }
        if (only_words)
        {
            if (auto status = append_unsigned(words); !status.ok())
            {
                return status;
            }
            return append("\n");
        }
        if (only_bytes)
        {
            if (auto status = append_unsigned(input.size()); !status.ok())
            {
                return status;
            }
            return append("\n");
        }
        if (auto status = append_padded_unsigned(lines, 4); !status.ok())
        {
            return status;
        }
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
        if (auto status = append_padded_unsigned(words, 4); !status.ok())
        {
            return status;
        }
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(input.size()); !status.ok())
        {
            return status;
        }
        return append("\n");
    }
    if (command == "head" || command == "tail")
    {
        if (!has_input)
        {
            return Status::invalid_argument("head/tail require pipeline or input redirection");
        }
        usize count = 10;
        if (!args.empty())
        {
            std::string_view number = args;
            if (first_word(args) == "-n")
            {
                number = after_first_word(args);
            }
            else if (number.size() > 2 && number[0] == '-' && number[1] == 'n')
            {
                number.remove_prefix(2);
            }
            auto parsed = shell_detail::parse_unsigned(number);
            if (!parsed)
            {
                return parsed.status();
            }
            count = static_cast<usize>(parsed.value());
        }

        usize total_lines = input.empty() ? 0 : 1;
        for (const auto ch : input)
        {
            if (ch == '\n')
            {
                ++total_lines;
            }
        }
        const usize first_line = command == "tail" && total_lines > count ? total_lines - count : 0;
        usize current_line = 0;
        usize emitted = 0;
        usize line_start = 0;
        for (usize i = 0; i <= input.size(); ++i)
        {
            if (i != input.size() && input[i] != '\n')
            {
                continue;
            }
            if (current_line >= first_line && emitted < count)
            {
                if (auto status = append(input.substr(line_start, i - line_start)); !status.ok())
                {
                    return status;
                }
                if (i < input.size() || !input.empty())
                {
                    if (auto status = append("\n"); !status.ok())
                    {
                        return status;
                    }
                }
                ++emitted;
            }
            ++current_line;
            line_start = i + 1;
            if (command == "head" && emitted >= count)
            {
                break;
            }
        }
        return Status::success();
    }
    return Status::not_found("filter not active");
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

Status KernelDebugShell::append_padded(std::string_view text, usize width)
{
    if (auto status = append(text); !status.ok())
    {
        return status;
    }
    for (usize i = text.size(); i < width; ++i)
    {
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status KernelDebugShell::append_padded_unsigned(u64 value, usize width)
{
    const auto digits = decimal_digits(value);
    for (usize i = digits; i < width; ++i)
    {
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
    }
    return append_unsigned(value);
}

Status KernelDebugShell::append_status_code(u32 value)
{
    return append_unsigned(value);
}

Status KernelDebugShell::append_node_type(fs::NodeType type)
{
    switch (type)
    {
    case fs::NodeType::directory:
        return append("dir");
    case fs::NodeType::regular:
        return append("file");
    case fs::NodeType::device:
        return append("dev");
    case fs::NodeType::symlink:
        return append("link");
    }
    return append("unknown");
}

Status KernelDebugShell::ensure_gui_surface()
{
    if (!gui_open_)
    {
        return Status::not_initialized("GUI shell is closed");
    }
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }

    auto &gui_module = kernel_->gui();
    auto &compositor = gui_module.compositor();
    if (compositor.crashed())
    {
        gui::GuiSupervisor supervisor{kernel_->kernel_modules(), gui_module};
        if (auto status = supervisor.tick(); !status.ok())
        {
            return status;
        }
        gui_surface_id_ = 0;
    }
    if (compositor.state() != gui::GuiState::running)
    {
        return Status::not_initialized("GUI compositor is not running");
    }
    if (auto status = ensure_gui_process(); !status.ok())
    {
        return status;
    }
    if (gui_surface_id_ != 0)
    {
        if (compositor.surface_info(gui_surface_id_))
        {
            return Status::success();
        }
        gui_surface_id_ = 0;
    }

    auto surface = compositor.create_surface(shell_gui_bounds, "oksh");
    if (!surface)
    {
        return surface.status();
    }
    gui_surface_id_ = surface.value();
    if (auto status = compositor.set_surface_app(gui_surface_id_, gui::TaskbarApp::debug_shell); !status.ok())
    {
        return status;
    }
    return record_gui_window();
}

Status KernelDebugShell::append_gui_history(std::string_view text)
{
    if (auto status = gui_history_.append(text); status.ok())
    {
        return Status::success();
    }

    gui_history_.clear();
    static_cast<void>(gui_history_.append("[shell history truncated]\n"));
    const auto tail = text.size() > shell_gui_history_keep ? text.substr(text.size() - shell_gui_history_keep) : text;
    return gui_history_.append(tail);
}

Status KernelDebugShell::append_gui_history_unsigned(u64 value)
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
            char ch = static_cast<char>('0' + digit);
            if (auto status = append_gui_history(std::string_view{&ch, 1}); !status.ok())
            {
                return status;
            }
            started = true;
        }
    }
    return Status::success();
}

Status KernelDebugShell::redraw_gui_terminal()
{
    if (auto status = ensure_gui_surface(); !status.ok())
    {
        return status;
    }

    auto &compositor = kernel_->gui().compositor();
    auto info = compositor.surface_info(gui_surface_id_);
    if (!info)
    {
        gui_surface_id_ = 0;
        return info.status();
    }
    const auto bounds = info.value().bounds;
    const auto total_rows = static_cast<usize>(bounds.height / gui::gui_glyph_height);
    const auto visible_rows =
        total_rows > shell_gui_content_row + 1 ? total_rows - shell_gui_content_row - 1 : static_cast<usize>(1);
    const auto text_columns = bounds.width > gui::gui_glyph_width
                                  ? static_cast<usize>((bounds.width / gui::gui_glyph_width) - 1)
                                  : static_cast<usize>(1);
    if (auto status = compositor.fill(gui_surface_id_, shell_gui_background); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.fill_rect(gui_surface_id_,
                                           gui::Rect{.x = 1,
                                                     .y = 1,
                                                     .width = bounds.width > 2 ? bounds.width - 2 : 1,
                                                     .height = gui::gui_glyph_height * 2 + 2},
                                           shell_gui_title_background);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.fill_rect(gui_surface_id_,
                                           gui::Rect{.x = 1,
                                                     .y = static_cast<i32>(gui::gui_glyph_height * 2 + 3),
                                                     .width = bounds.width > 2 ? bounds.width - 2 : 1,
                                                     .height = 1},
                                           shell_gui_title_border);
        !status.ok())
    {
        return status;
    }
    FixedString<2304> terminal;
    if (auto status = terminal.append(gui_history_.view()); !status.ok())
    {
        return status;
    }
    if (foreground_process_id_ == 0)
    {
        if (auto status = terminal.append("ok> "); !status.ok())
        {
            return status;
        }
        if (auto status = terminal.append(gui_input_line_.view()); !status.ok())
        {
            return status;
        }
    }

    const auto text_rows = visual_line_count(terminal.view(), text_columns);
    const auto max_scroll = text_rows > visible_rows ? text_rows - visible_rows : 0;
    if (gui_scroll_rows_ > max_scroll)
    {
        gui_scroll_rows_ = max_scroll;
    }
    const auto first_row = text_rows > visible_rows + gui_scroll_rows_
                               ? text_rows - visible_rows - gui_scroll_rows_
                               : 0;
    const auto visible_start = visual_line_offset(terminal.view(), text_columns, first_row);
    const auto visible = terminal.view().substr(visible_start);
    if (auto status = compositor.draw_text(gui_surface_id_, 1, shell_gui_content_row, visible, shell_gui_foreground,
                                           shell_gui_background);
        !status.ok())
    {
        return status;
    }
    if (auto status =
            compositor.draw_text(gui_surface_id_, 1, 1, "oksh", shell_gui_prompt, shell_gui_title_background);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.present(); !status.ok())
    {
        return status;
    }
    ++gui_render_count_;
    return Status::success();
}

Status KernelDebugShell::render_to_gui(std::string_view command_line, std::string_view output)
{
    if (!gui_open_)
    {
        return Status::success();
    }
    bool clears_screen = false;
    for (const auto value : output)
    {
        if (value == '\f')
        {
            clears_screen = true;
            break;
        }
    }
    if (clears_screen)
    {
        gui_history_.clear();
        gui_input_line_.clear();
        gui_scroll_rows_ = 0;
        return redraw_gui_terminal();
    }
    gui_scroll_rows_ = 0;
    gui_input_line_.clear();

    if (auto status = append_gui_history("ok> "); !status.ok())
    {
        return status;
    }
    if (auto status = append_gui_history(command_line); !status.ok())
    {
        return status;
    }
    if (auto status = append_gui_history("\n"); !status.ok())
    {
        return status;
    }

    usize segment_start = 0;
    for (usize i = 0; i <= output.size(); ++i)
    {
        if (i == output.size() || output[i] == '\f')
        {
            if (i > segment_start)
            {
                if (auto status = append_gui_history(output.substr(segment_start, i - segment_start)); !status.ok())
                {
                    return status;
                }
            }
            segment_start = i + 1;
        }
    }

    return redraw_gui_terminal();
}

Status KernelDebugShell::append_session_user()
{
    return append(session_user_name_.view());
}

Result<std::string_view> KernelDebugShell::resolve_path(std::string_view path)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    path = trim(path);
    FixedString<96> joined;
    if (path.empty() || path == ".")
    {
        path = kernel_->posix().getcwd();
    }
    if (!path.empty() && path.front() != '/')
    {
        if (auto status = joined.append(kernel_->posix().getcwd()); !status.ok())
        {
            return status;
        }
        if (joined.view() != "/")
        {
            if (auto status = joined.append('/'); !status.ok())
            {
                return status;
            }
        }
        if (auto status = joined.append(path); !status.ok())
        {
            return status;
        }
        path = joined.view();
    }

    path_buffer_.clear();
    if (auto status = path_buffer_.append('/'); !status.ok())
    {
        return status;
    }
    usize cursor = 0;
    while (cursor < path.size())
    {
        while (cursor < path.size() && path[cursor] == '/')
        {
            ++cursor;
        }
        const auto start = cursor;
        while (cursor < path.size() && path[cursor] != '/')
        {
            ++cursor;
        }
        const auto segment = path.substr(start, cursor - start);
        if (segment.empty() || segment == ".")
        {
            continue;
        }
        if (segment == "..")
        {
            while (path_buffer_.size() > 1 && path_buffer_.view().back() != '/')
            {
                path_buffer_.pop_back();
            }
            if (path_buffer_.size() > 1)
            {
                path_buffer_.pop_back();
            }
            continue;
        }
        if (path_buffer_.view() != "/")
        {
            if (auto status = path_buffer_.append('/'); !status.ok())
            {
                return status;
            }
        }
        if (auto status = path_buffer_.append(segment); !status.ok())
        {
            return status;
        }
    }
    if (path_buffer_.empty())
    {
        if (auto status = path_buffer_.append('/'); !status.ok())
        {
            return status;
        }
    }
    return path_buffer_.view();
}

} // namespace ok
