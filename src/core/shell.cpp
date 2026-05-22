#include "ok/core/shell.hpp"

#include "ok/core/kernel.hpp"
#include "ok/fs/ext4.hpp"

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

std::string_view first_word(std::string_view value)
{
    value = trim(value);
    usize size = 0;
    while (size < value.size() && value[size] != ' ' && value[size] != '\t')
    {
        ++size;
    }
    return value.substr(0, size);
}

std::string_view after_first_word(std::string_view value)
{
    value = trim(value);
    auto word = first_word(value);
    value.remove_prefix(word.size());
    return trim(value);
}

std::span<const std::byte> as_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

Result<u64> parse_unsigned(std::string_view text)
{
    text = trim(text);
    if (text.empty())
    {
        return Status::invalid_argument("expected unsigned integer");
    }
    u64 value = 0;
    for (const auto ch : text)
    {
        if (ch < '0' || ch > '9')
        {
            return Status::invalid_argument("invalid unsigned integer");
        }
        value = value * 10 + static_cast<u64>(ch - '0');
    }
    return value;
}

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

Status KernelDebugShell::append_session_user()
{
    switch (session_user_)
    {
    case SessionUser::kernel:
        return append("kernel");
    case SessionUser::root:
        return append("root");
    case SessionUser::user:
        return append("user");
    }
    return append("unknown");
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

Status KernelDebugShell::command_echo(std::string_view text)
{
    if (auto status = append(text); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_pwd()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (auto status = append(kernel_->posix().getcwd()); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_cd(std::string_view path)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    auto resolved = resolve_path(path);
    if (!resolved)
    {
        return resolved.status();
    }
    return kernel_->posix().chdir(resolved.value());
}

Status KernelDebugShell::command_ls(std::string_view path)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    auto resolved = resolve_path(path);
    if (!resolved)
    {
        return resolved.status();
    }
    auto listing = kernel_->vfs().list(resolved.value());
    if (!listing)
    {
        return listing.status();
    }
    for (usize i = 0; i < listing.value().count; ++i)
    {
        const auto &entry = listing.value().entries[i];
        if (auto status = append_node_type(entry.metadata.type); !status.ok())
        {
            return status;
        }
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
        if (auto status = append(entry.name.view()); !status.ok())
        {
            return status;
        }
        if (entry.metadata.type == fs::NodeType::directory)
        {
            if (auto status = append("/"); !status.ok())
            {
                return status;
            }
        }
        if (auto status = append(" size="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(entry.metadata.size); !status.ok())
        {
            return status;
        }
        if (auto status = append("\n"); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status KernelDebugShell::command_cat(std::string_view path)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (trim(path).empty())
    {
        return Status::invalid_argument("cat requires a path");
    }
    auto resolved = resolve_path(path);
    if (!resolved)
    {
        return resolved.status();
    }
    auto file = kernel_->vfs().read_file(resolved.value());
    if (!file)
    {
        return file.status();
    }
    for (usize i = 0; i < file.value().size; ++i)
    {
        if (auto status = output_.append(static_cast<char>(file.value().data[i])); !status.ok())
        {
            return status;
        }
    }
    if (file.value().size == 0 || output_.view().back() != '\n')
    {
        return append("\n");
    }
    return Status::success();
}

Status KernelDebugShell::command_touch(std::string_view path)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (trim(path).empty())
    {
        return Status::invalid_argument("touch requires a path");
    }
    auto resolved = resolve_path(path);
    if (!resolved)
    {
        return resolved.status();
    }
    auto fd = kernel_->posix().open(resolved.value(), posix::o_CREAT | posix::o_RDWR);
    if (!fd)
    {
        return fd.status();
    }
    return kernel_->posix().close(fd.value());
}

Status KernelDebugShell::command_mkdir(std::string_view path)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (trim(path).empty())
    {
        return Status::invalid_argument("mkdir requires a path");
    }
    auto resolved = resolve_path(path);
    if (!resolved)
    {
        return resolved.status();
    }
    return kernel_->posix().mkdir(resolved.value());
}

Status KernelDebugShell::command_rm(std::string_view path)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (trim(path).empty())
    {
        return Status::invalid_argument("rm requires a path");
    }
    auto resolved = resolve_path(path);
    if (!resolved)
    {
        return resolved.status();
    }
    return kernel_->posix().unlink(resolved.value());
}

Status KernelDebugShell::command_stat(std::string_view path)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (trim(path).empty())
    {
        return Status::invalid_argument("stat requires a path");
    }
    auto resolved = resolve_path(path);
    if (!resolved)
    {
        return resolved.status();
    }
    auto stat = kernel_->posix().stat(resolved.value());
    if (!stat)
    {
        return stat.status();
    }
    if (auto status = append_node_type(stat.value().type); !status.ok())
    {
        return status;
    }
    if (auto status = append(" size="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stat.value().size); !status.ok())
    {
        return status;
    }
    if (auto status = append(" mode="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stat.value().mode); !status.ok())
    {
        return status;
    }
    if (auto status = append(" uid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stat.value().uid); !status.ok())
    {
        return status;
    }
    if (auto status = append(" gid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stat.value().gid); !status.ok())
    {
        return status;
    }
    if (auto status = append(" links="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stat.value().link_count); !status.ok())
    {
        return status;
    }
    if (auto status = append(" blocks="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stat.value().blocks); !status.ok())
    {
        return status;
    }
    if (auto status = append(" blksize="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stat.value().block_size); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_whoami()
{
    if (auto status = append_session_user(); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_id()
{
    if (auto status = append("uid="); !status.ok())
    {
        return status;
    }
    switch (session_user_)
    {
    case SessionUser::kernel:
        if (auto status = append("0(kernel) gid=0(kernel) kernel_space=1\n"); !status.ok())
        {
            return status;
        }
        break;
    case SessionUser::root:
        if (auto status = append("0(root) gid=0(root) kernel_space=0\n"); !status.ok())
        {
            return status;
        }
        break;
    case SessionUser::user:
        if (auto status = append("1000(user) gid=1000(user) kernel_space=0\n"); !status.ok())
        {
            return status;
        }
        break;
    }
    return Status::success();
}

Status KernelDebugShell::command_su(std::string_view user)
{
    user = trim(user);
    if (user == "kernel")
    {
        session_user_ = SessionUser::kernel;
    }
    else if (user == "root")
    {
        session_user_ = SessionUser::root;
    }
    else if (user == "user")
    {
        session_user_ = SessionUser::user;
    }
    else
    {
        return Status::not_found("unknown debug shell user");
    }
    return command_whoami();
}

Status KernelDebugShell::command_disk()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto geometry = kernel_->disk().geometry();
    if (auto status = append("disk="); !status.ok())
    {
        return status;
    }
    if (auto status = append(kernel_->disk_name()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" blocks="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(geometry.block_count); !status.ok())
    {
        return status;
    }
    if (auto status = append(" block_size="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(geometry.block_size); !status.ok())
    {
        return status;
    }
    if (auto status = append(" writable="); !status.ok())
    {
        return status;
    }
    if (auto status = append(geometry.writable ? "1" : "0"); !status.ok())
    {
        return status;
    }

    auto info = kernel_->simplefs().info();
    if (!info)
    {
        return append(" simplefs=unmounted\n");
    }
    if (auto status = append(" simplefs="); !status.ok())
    {
        return status;
    }
    if (auto status = append(info.value().label.view()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" files="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(info.value().file_count); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_mkfs(std::string_view label)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    label = trim(label);
    if (label.empty())
    {
        label = "okroot";
    }
    if (auto status = kernel_->simplefs().format(kernel_->disk(), label); !status.ok())
    {
        return status;
    }
    return append("formatted simplefs\n");
}

Status KernelDebugShell::command_simplefs(std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto subcommand = first_word(args);
    const auto rest = after_first_word(args);
    if (subcommand.empty() || subcommand == "info")
    {
        auto info = kernel_->simplefs().info();
        if (!info)
        {
            return info.status();
        }
        if (auto status = append("label="); !status.ok())
        {
            return status;
        }
        if (auto status = append(info.value().label.view()); !status.ok())
        {
            return status;
        }
        if (auto status = append(" files="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(info.value().file_count); !status.ok())
        {
            return status;
        }
        if (auto status = append(" blocks="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(info.value().block_count); !status.ok())
        {
            return status;
        }
        return append("\n");
    }
    if (subcommand == "ls")
    {
        auto listing = kernel_->simplefs().list_root();
        if (!listing)
        {
            return listing.status();
        }
        for (usize i = 0; i < listing.value().count; ++i)
        {
            const auto &entry = listing.value().entries[i];
            if (auto status = append_node_type(entry.metadata.type); !status.ok())
            {
                return status;
            }
            if (auto status = append(" "); !status.ok())
            {
                return status;
            }
            if (auto status = append(entry.name.view()); !status.ok())
            {
                return status;
            }
            if (auto status = append(" size="); !status.ok())
            {
                return status;
            }
            if (auto status = append_unsigned(entry.metadata.size); !status.ok())
            {
                return status;
            }
            if (auto status = append("\n"); !status.ok())
            {
                return status;
            }
        }
        return Status::success();
    }
    if (subcommand == "touch")
    {
        if (trim(rest).empty())
        {
            return Status::invalid_argument("sfs touch requires a path");
        }
        return kernel_->simplefs().create(rest, fs::NodeType::regular);
    }
    if (subcommand == "rm")
    {
        if (trim(rest).empty())
        {
            return Status::invalid_argument("sfs rm requires a path");
        }
        return kernel_->simplefs().unlink(rest);
    }
    if (subcommand == "cat")
    {
        if (trim(rest).empty())
        {
            return Status::invalid_argument("sfs cat requires a path");
        }
        auto file = kernel_->simplefs().read_file(rest);
        if (!file)
        {
            return file.status();
        }
        for (usize i = 0; i < file.value().size; ++i)
        {
            if (auto status = output_.append(static_cast<char>(file.value().data[i])); !status.ok())
            {
                return status;
            }
        }
        if (file.value().size == 0 || output_.view().back() != '\n')
        {
            return append("\n");
        }
        return Status::success();
    }
    if (subcommand == "stat")
    {
        if (trim(rest).empty())
        {
            return Status::invalid_argument("sfs stat requires a path");
        }
        auto stat = kernel_->simplefs().stat(rest);
        if (!stat)
        {
            return stat.status();
        }
        if (auto status = append_node_type(stat.value().type); !status.ok())
        {
            return status;
        }
        if (auto status = append(" size="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(stat.value().size); !status.ok())
        {
            return status;
        }
        return append("\n");
    }
    if (subcommand == "write")
    {
        const auto path = first_word(rest);
        const auto text = after_first_word(rest);
        if (path.empty())
        {
            return Status::invalid_argument("sfs write requires a path and text");
        }
        auto stat = kernel_->simplefs().stat(path);
        if (!stat)
        {
            if (stat.status().code() != StatusCode::not_found)
            {
                return stat.status();
            }
            if (auto status = kernel_->simplefs().create(path, fs::NodeType::regular); !status.ok())
            {
                return status;
            }
        }
        return kernel_->simplefs().write_file(path, as_bytes(text));
    }
    return Status::invalid_argument("unknown sfs subcommand");
}

Status KernelDebugShell::command_ext4(std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto subcommand = first_word(args);
    if (subcommand.empty() || subcommand == "status")
    {
        return append("ext4=read-only-superblock block-device-mount=1 extents=1 journal=replay-pending\n");
    }
    if (subcommand == "disk")
    {
        fs::Ext4Volume volume;
        if (auto status = volume.mount(kernel_->disk()); !status.ok())
        {
            return status;
        }
        auto info = volume.info();
        if (!info)
        {
            return info.status();
        }
        if (auto status = append("label="); !status.ok())
        {
            return status;
        }
        if (auto status = append(info.value().volume_name.view()); !status.ok())
        {
            return status;
        }
        if (auto status = append(" block_size="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(info.value().block_size); !status.ok())
        {
            return status;
        }
        if (auto status = append(" blocks="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(info.value().block_count_low); !status.ok())
        {
            return status;
        }
        return append("\n");
    }
    return Status::invalid_argument("unknown ext4 subcommand");
}

Status KernelDebugShell::command_net(std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto subcommand = first_word(args);
    const auto rest = after_first_word(args);
    if (subcommand.empty() || subcommand == "status")
    {
        const auto stats = kernel_->network().stats();
        if (auto status = append("ip=127.0.0.1 udp_queue="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(kernel_->network().udp_queued()); !status.ok())
        {
            return status;
        }
        if (auto status = append(" tcp_listeners="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(kernel_->network().tcp_listener_count()); !status.ok())
        {
            return status;
        }
        if (auto status = append(" ipv4_tx="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(stats.ipv4_tx); !status.ok())
        {
            return status;
        }
        if (auto status = append(" ipv4_rx="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(stats.ipv4_rx); !status.ok())
        {
            return status;
        }
        return append("\n");
    }
    if (subcommand == "udp")
    {
        const auto port_text = first_word(rest);
        const auto payload = after_first_word(rest);
        auto port = parse_unsigned(port_text);
        if (!port)
        {
            return port.status();
        }
        if (port.value() > 0xffffu)
        {
            return Status::invalid_argument("UDP port out of range");
        }
        return kernel_->network().send_udp(net::UdpEndpoint{.address = kernel_->network().local_address(), .port = 30000},
                                           net::UdpEndpoint{.address = kernel_->network().local_address(),
                                                            .port = static_cast<u16>(port.value())},
                                           as_bytes(payload));
    }
    if (subcommand == "recv")
    {
        auto datagram = kernel_->network().receive_udp();
        if (!datagram)
        {
            return datagram.status();
        }
        if (auto status = append("udp port="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(datagram.value().destination.port); !status.ok())
        {
            return status;
        }
        if (auto status = append(" payload="); !status.ok())
        {
            return status;
        }
        for (usize i = 0; i < datagram.value().payload_size; ++i)
        {
            if (auto status = output_.append(static_cast<char>(datagram.value().payload[i])); !status.ok())
            {
                return status;
            }
        }
        return append("\n");
    }
    if (subcommand == "listen")
    {
        auto port = parse_unsigned(rest);
        if (!port)
        {
            return port.status();
        }
        if (port.value() > 0xffffu)
        {
            return Status::invalid_argument("TCP port out of range");
        }
        return kernel_->network().listen_tcp(static_cast<u16>(port.value()));
    }
    if (subcommand == "tcp")
    {
        auto port = parse_unsigned(rest);
        if (!port)
        {
            return port.status();
        }
        if (port.value() > 0xffffu)
        {
            return Status::invalid_argument("TCP port out of range");
        }
        auto connection =
            kernel_->network().connect_tcp(net::UdpEndpoint{.address = kernel_->network().local_address(),
                                                            .port = static_cast<u16>(port.value())},
                                           40000);
        if (!connection)
        {
            return connection.status();
        }
        if (auto status = append("tcp state="); !status.ok())
        {
            return status;
        }
        if (auto status = append(net::tcp_state_name(connection.value().state)); !status.ok())
        {
            return status;
        }
        return append("\n");
    }
    return Status::invalid_argument("unknown net subcommand");
}

} // namespace ok
