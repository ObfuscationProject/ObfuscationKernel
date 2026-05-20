#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/fs/vfs.hpp"

#include <string_view>

namespace ok
{

class Kernel;

class KernelDebugShell final
{
  public:
    Status attach(Kernel &kernel);
    Result<std::string_view> execute(std::string_view line);

  private:
    enum class SessionUser : u8
    {
        kernel,
        root,
        user,
    };

    Status append(std::string_view text);
    Status append_unsigned(u64 value);
    Status append_node_type(fs::NodeType type);
    Status append_session_user();
    [[nodiscard]] Result<std::string_view> resolve_path(std::string_view path);
    Status dispatch_command(std::string_view command_line);
    Status command_help();
    Status command_true();
    Status command_false();
    Status command_noop();
    Status command_clear();
    Status command_uname();
    Status command_status();
    Status command_memory();
    Status command_processes();
    Status command_drivers();
    Status command_filesystem();
    Status command_posix();
    Status command_tests();
    Status command_echo(std::string_view text);
    Status command_pwd();
    Status command_cd(std::string_view path);
    Status command_ls(std::string_view path);
    Status command_cat(std::string_view path);
    Status command_touch(std::string_view path);
    Status command_mkdir(std::string_view path);
    Status command_rm(std::string_view path);
    Status command_stat(std::string_view path);
    Status command_whoami();
    Status command_id();
    Status command_su(std::string_view user);
    Status command_disk();
    Status command_mkfs(std::string_view label);
    Status command_simplefs(std::string_view args);
    Status command_ext4(std::string_view args);
    Status command_net(std::string_view args);

    Kernel *kernel_{nullptr};
    SessionUser session_user_{SessionUser::kernel};
    FixedString<96> path_buffer_{};
    FixedString<4096> output_{};
};

} // namespace ok
