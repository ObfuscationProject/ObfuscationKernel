#include "ok/core/shell.hpp"

#include "ok/core/kernel.hpp"
#include "ok/fs/ext4.hpp"
#include "shell_private.hpp"

namespace ok
{

using shell_detail::after_first_word;
using shell_detail::as_bytes;
using shell_detail::first_word;
using shell_detail::trim;

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
    const auto stats = kernel_->disk().io_stats();
    if (auto status = append(" reads="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stats.read_operations); !status.ok())
    {
        return status;
    }
    if (auto status = append(" writes="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stats.write_operations); !status.ok())
    {
        return status;
    }
    if (auto status = append(" read_bytes="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stats.bytes_read); !status.ok())
    {
        return status;
    }
    if (auto status = append(" write_bytes="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(stats.bytes_written); !status.ok())
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

} // namespace ok
