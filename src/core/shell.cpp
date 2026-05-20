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

} // namespace

Status KernelDebugShell::attach(Kernel &kernel)
{
    kernel_ = &kernel;
    return Status::success();
}

Result<std::string_view> KernelDebugShell::execute(std::string_view line)
{
    output_.clear();
    const auto command_line = trim(line);
    if (command_line.empty())
    {
        return output_.view();
    }
    const auto command = first_word(command_line);
    const auto args = after_first_word(command_line);
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
    else if (command == "echo")
    {
        status = command_echo(args);
    }
    else if (command == "pwd")
    {
        status = command_pwd();
    }
    else if (command == "cd")
    {
        status = command_cd(args);
    }
    else if (command == "ls")
    {
        status = command_ls(args);
    }
    else if (command == "cat")
    {
        status = command_cat(args);
    }
    else if (command == "touch")
    {
        status = command_touch(args);
    }
    else if (command == "mkdir")
    {
        status = command_mkdir(args);
    }
    else if (command == "rm")
    {
        status = command_rm(args);
    }
    else if (command == "stat")
    {
        status = command_stat(args);
    }
    else if (command == "whoami")
    {
        status = command_whoami();
    }
    else if (command == "id")
    {
        status = command_id();
    }
    else if (command == "su")
    {
        status = command_su(args);
    }
    else if (command == "disk")
    {
        status = command_disk();
    }
    else if (command == "mkfs")
    {
        status = command_mkfs(args);
    }
    else if (command == "sfs")
    {
        status = command_simplefs(args);
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
        "help status mem ps drivers fs posix test echo pwd cd ls cat touch mkdir rm stat whoami id su disk mkfs sfs\n");
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
    if (auto status = append("disk=ram-block0 blocks="); !status.ok())
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

} // namespace ok
