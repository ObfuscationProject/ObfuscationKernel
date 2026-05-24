#include "ok/core/shell.hpp"

#include "ok/core/kernel.hpp"
#include "shell_private.hpp"

namespace ok
{

using shell_detail::as_bytes;
using shell_detail::after_first_word;
using shell_detail::first_word;
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

    struct LsOptions
    {
        bool all{false};
        bool human{false};
        bool long_format{false};
        std::string_view path{};
    };

    LsOptions options{};
    auto remaining = trim(path);
    while (!remaining.empty())
    {
        const auto word = first_word(remaining);
        remaining = after_first_word(remaining);
        if (word.size() > 1 && word.front() == '-')
        {
            for (usize i = 1; i < word.size(); ++i)
            {
                switch (word[i])
                {
                case 'a':
                    options.all = true;
                    break;
                case 'h':
                    options.human = true;
                    break;
                case 'l':
                    options.long_format = true;
                    break;
                default:
                    return Status::unsupported("unsupported ls option");
                }
            }
            continue;
        }
        if (!options.path.empty())
        {
            return Status::unsupported("ls supports one path");
        }
        options.path = word;
    }

    auto resolved = resolve_path(options.path);
    if (!resolved)
    {
        return resolved.status();
    }
    auto listing = kernel_->vfs().list(resolved.value());
    if (!listing)
    {
        return listing.status();
    }

    auto append_char = [this](char value) {
        return output_.append(value);
    };
    auto append_mode = [&](fs::Metadata metadata) -> Status {
        char type = '-';
        switch (metadata.type)
        {
        case fs::NodeType::directory:
            type = 'd';
            break;
        case fs::NodeType::regular:
            type = '-';
            break;
        case fs::NodeType::device:
            type = 'c';
            break;
        case fs::NodeType::symlink:
            type = 'l';
            break;
        }
        if (auto status = append_char(type); !status.ok())
        {
            return status;
        }
        constexpr u32 bits[] = {0400u, 0200u, 0100u, 0040u, 0020u, 0010u, 0004u, 0002u, 0001u};
        constexpr char chars[] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
        for (usize i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i)
        {
            if (auto status = append_char((metadata.mode & bits[i]) != 0 ? chars[i] : '-'); !status.ok())
            {
                return status;
            }
        }
        return Status::success();
    };
    auto append_size = [&](usize size) -> Status {
        if (!options.human)
        {
            return append_unsigned(size);
        }
        if (size >= 1024 * 1024)
        {
            if (auto status = append_unsigned((size + 1024 * 1024 - 1) / (1024 * 1024)); !status.ok())
            {
                return status;
            }
            return append("M");
        }
        if (size >= 1024)
        {
            if (auto status = append_unsigned((size + 1023) / 1024); !status.ok())
            {
                return status;
            }
            return append("K");
        }
        if (auto status = append_unsigned(size); !status.ok())
        {
            return status;
        }
        return append("B");
    };
    auto append_name = [&](std::string_view name, fs::Metadata metadata) -> Status {
        if (auto status = append(name); !status.ok())
        {
            return status;
        }
        if (metadata.type == fs::NodeType::directory)
        {
            return append("/");
        }
        return Status::success();
    };
    auto append_entry = [&](std::string_view name, fs::Metadata metadata) -> Status {
        if (options.long_format)
        {
            if (auto status = append_mode(metadata); !status.ok())
            {
                return status;
            }
            if (auto status = append(" "); !status.ok())
            {
                return status;
            }
            if (auto status = append_unsigned(metadata.link_count); !status.ok())
            {
                return status;
            }
            if (auto status = append(" "); !status.ok())
            {
                return status;
            }
            if (auto status = append_unsigned(metadata.uid); !status.ok())
            {
                return status;
            }
            if (auto status = append(" "); !status.ok())
            {
                return status;
            }
            if (auto status = append_unsigned(metadata.gid); !status.ok())
            {
                return status;
            }
            if (auto status = append(" "); !status.ok())
            {
                return status;
            }
            if (auto status = append_size(metadata.size); !status.ok())
            {
                return status;
            }
            if (auto status = append(" "); !status.ok())
            {
                return status;
            }
        }
        if (auto status = append_name(name, metadata); !status.ok())
        {
            return status;
        }
        return append("\n");
    };

    if (options.all)
    {
        auto metadata = kernel_->vfs().stat(resolved.value());
        if (!metadata)
        {
            return metadata.status();
        }
        if (auto status = append_entry(".", metadata.value()); !status.ok())
        {
            return status;
        }
        if (auto status = append_entry("..", metadata.value()); !status.ok())
        {
            return status;
        }
    }
    for (usize i = 0; i < listing.value().count; ++i)
    {
        const auto &entry = listing.value().entries[i];
        const auto name = entry.name.view();
        if (!options.all && !name.empty() && name.front() == '.')
        {
            continue;
        }
        if (auto status = append_entry(name, entry.metadata); !status.ok())
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

Status KernelDebugShell::command_file_manager(std::string_view path)
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
    if (auto status = kernel_->file_manager().open(kernel_->gui().compositor(), kernel_->vfs(), resolved.value());
        !status.ok())
    {
        return status;
    }
    if (auto status = append("file manager: "); !status.ok())
    {
        return status;
    }
    if (auto status = append(resolved.value()); !status.ok())
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
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto credentials = kernel_->posix().user_credentials();
    if (auto status = append("uid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(credentials.uid); !status.ok())
    {
        return status;
    }
    if (auto status = append("("); !status.ok())
    {
        return status;
    }
    if (auto status = append_session_user(); !status.ok())
    {
        return status;
    }
    if (auto status = append(") gid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(credentials.gid); !status.ok())
    {
        return status;
    }
    if (auto status = append(" euid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(credentials.euid); !status.ok())
    {
        return status;
    }
    if (auto status = append(" egid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(credentials.egid); !status.ok())
    {
        return status;
    }
    if (auto status = append(" kernel_space="); !status.ok())
    {
        return status;
    }
    if (auto status = append(credentials.kernel_space ? "1" : "0"); !status.ok())
    {
        return status;
    }
    return append("\n");
}

Status KernelDebugShell::command_su(std::string_view user)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    user = trim(user);
    if (user.empty())
    {
        user = "root";
    }
    auto active = kernel_->posix().user_credentials();
    if (auto status = kernel_->user_space().switch_credentials(active, user); !status.ok())
    {
        return status;
    }
    if (auto status = kernel_->posix().set_credentials(active); !status.ok())
    {
        return status;
    }
    if (auto status = session_user_name_.assign(user); !status.ok())
    {
        return status;
    }
    return command_whoami();
}

} // namespace ok
