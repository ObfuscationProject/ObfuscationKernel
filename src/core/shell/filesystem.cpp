#include "ok/core/shell.hpp"

#include "ok/core/kernel.hpp"
#include "shell_private.hpp"

namespace ok
{

using shell_detail::as_bytes;
using shell_detail::trim;

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

} // namespace ok
