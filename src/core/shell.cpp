#include "ok/core/shell.hpp"

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

usize find_output_redirection(std::string_view value)
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
        if (!single_quote && !double_quote && ch == '>')
        {
            return i;
        }
    }
    return value.size();
}

ShellOperator operator_after(std::string_view value, usize index)
{
    if (index >= value.size() || value[index] == ';')
    {
        return ShellOperator::always;
    }
    return value[index] == '&' ? ShellOperator::and_if : ShellOperator::or_if;
}

usize operator_width(std::string_view value, usize index)
{
    if (index >= value.size())
    {
        return 0;
    }
    return value[index] == ';' ? 1 : 2;
}

constexpr gui::Rect shell_gui_bounds{.x = 4, .y = 4, .width = 124, .height = 72};
constexpr u32 shell_gui_background = 0xff061018u;
constexpr u32 shell_gui_foreground = 0xffd8f3ffu;
constexpr u32 shell_gui_prompt = 0xfff4d35eu;
constexpr usize shell_gui_rows = shell_gui_bounds.height / gui::gui_glyph_height - 3;
constexpr usize shell_gui_history_keep = 1800;

std::string_view tail_lines(std::string_view text, usize max_lines)
{
    if (max_lines == 0)
    {
        return {};
    }
    usize lines = 0;
    for (usize i = text.size(); i != 0; --i)
    {
        if (text[i - 1] == '\n')
        {
            ++lines;
            if (lines > max_lines)
            {
                return text.substr(i);
            }
        }
    }
    return text;
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
    auto remaining = trim(strip_comment(line));
    if (remaining.empty())
    {
        return output_.view();
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
    return output_.view();
}

Status KernelDebugShell::dispatch_command(std::string_view command_line)
{
    const auto redirect = find_output_redirection(command_line);
    if (redirect < command_line.size())
    {
        const auto producer = trim(command_line.substr(0, redirect));
        const auto target = trim(command_line.substr(redirect + 1));
        if (producer.empty() || target.empty())
        {
            return Status::invalid_argument("output redirection requires a command and path");
        }
        if (find_output_redirection(target) < target.size())
        {
            return Status::invalid_argument("multiple output redirections are not supported");
        }

        const auto output_start = output_.size();
        const auto producer_status = dispatch_command(producer);
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

        auto resolved = resolve_path(target);
        if (!resolved)
        {
            return resolved.status();
        }
        auto fd = kernel_->posix().open(resolved.value(), posix::o_CREAT | posix::o_WRONLY | posix::o_TRUNC);
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

    const auto command = first_word(command_line);
    const auto args = after_first_word(command_line);
    if (command.empty())
    {
        return Status::success();
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
        return command_processes();
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
    else if (command == "stat")
    {
        return command_stat(args);
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
    else
    {
        return Status::not_found("command not found");
    }
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
    return Status::success();
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

Status KernelDebugShell::redraw_gui_terminal()
{
    if (auto status = ensure_gui_surface(); !status.ok())
    {
        return status;
    }

    auto &compositor = kernel_->gui().compositor();
    if (auto status = compositor.fill(gui_surface_id_, shell_gui_background); !status.ok())
    {
        return status;
    }
    const auto visible = tail_lines(gui_history_.view(), shell_gui_rows);
    if (auto status = compositor.draw_text(gui_surface_id_, 1, 2, visible, shell_gui_foreground, shell_gui_background);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.draw_text(gui_surface_id_, 1, 1, "oksh", shell_gui_prompt, shell_gui_background);
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
    }

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
    if (path.empty() || path == ".")
    {
        return kernel_->posix().getcwd();
    }
    if (path == "..")
    {
        const auto cwd = kernel_->posix().getcwd();
        if (cwd == "/" || cwd.empty())
        {
            return std::string_view{"/"};
        }
        path_buffer_.clear();
        usize end = cwd.size();
        while (end > 1 && cwd[end - 1] != '/')
        {
            --end;
        }
        if (end <= 1)
        {
            return std::string_view{"/"};
        }
        if (auto status = path_buffer_.assign(cwd.substr(0, end - 1)); !status.ok())
        {
            return status;
        }
        return path_buffer_.view();
    }
    if (path.front() == '/')
    {
        return path;
    }

    path_buffer_.clear();
    const auto cwd = kernel_->posix().getcwd();
    if (auto status = path_buffer_.append(cwd); !status.ok())
    {
        return status;
    }
    if (path_buffer_.view() != "/")
    {
        if (auto status = path_buffer_.append('/'); !status.ok())
        {
            return status;
        }
    }
    if (auto status = path_buffer_.append(path); !status.ok())
    {
        return status;
    }
    return path_buffer_.view();
}

} // namespace ok
