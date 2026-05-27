#include "ok/apps/shell.hpp"

#include "ok/core/kernel.hpp"
#include "shell_private.hpp"

namespace ok
{
namespace
{

using shell_detail::first_word;

std::string_view process_state_label(sched::ProcessState state)
{
    switch (state)
    {
    case sched::ProcessState::created:
        return "N";
    case sched::ProcessState::runnable:
    case sched::ProcessState::running:
        return "R";
    case sched::ProcessState::blocked:
        return "S";
    case sched::ProcessState::exited:
        return "Z";
    }
    return "?";
}

std::string_view process_tty_label(const sched::ProcessControlBlock &process)
{
    return process.background() ? "?" : "tty0";
}

std::string_view trim_ascii(std::string_view value)
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

bool parse_unsigned(std::string_view value, u64 &out)
{
    value = trim_ascii(value);
    if (value.empty())
    {
        return false;
    }
    u64 parsed = 0;
    for (const auto ch : value)
    {
        if (ch < '0' || ch > '9')
        {
            return false;
        }
        const auto digit = static_cast<u64>(ch - '0');
        if (parsed > ((__UINT64_MAX__ - digit) / 10u))
        {
            return false;
        }
        parsed = parsed * 10u + digit;
    }
    out = parsed;
    return true;
}

bool known_shell_command(std::string_view command)
{
    for (const auto *name :
         {"help",   "true",   "false",   ":",      "clear", "uname", "status", "system", "mem",   "ps",
          "drivers", "fs",     "posix",   "test",   "echo",  "pwd",   "cd",     "ls",    "cat",
          "cp",     "mv",     "touch",   "mkdir",  "rm",    "rmdir", "stat",   "chmod", "chown",
          "users",  "kill",   "shutdown", "poweroff", "halt", "reboot", "whoami", "id",    "su",
          "exit",   "disk",   "mkfs",    "sfs",    "ext4",  "net",   "fm",     "fileman",
          "top",    "taskman", "history", "env",    "export", "unset", "type",   "which", "grep",
          "wc",     "head",   "tail"})
    {
        if (command == name)
        {
            return true;
        }
    }
    return false;
}

} // namespace

Status KernelDebugShell::command_help()
{
    return append(
        "help true false : clear uname status system mem ps drivers fs posix test echo pwd cd ls cat cp mv touch mkdir rm "
        "rmdir stat chmod chown users kill shutdown poweroff halt reboot whoami id su exit disk mkfs sfs ext4 net "
        "fm fileman top taskman history env export unset type which grep wc head tail\n");
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

Status KernelDebugShell::command_system(std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto mode = first_word(args);
    if (!mode.empty() && mode != "tui" && mode != "status")
    {
        return Status::unsupported("system supports: system [tui|status]");
    }

    if (auto status = append("ObfuscationOS mode=tui login="); !status.ok())
    {
        return status;
    }
    if (auto status = append(session_user_name_.view()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" gui="); !status.ok())
    {
        return status;
    }
    if (auto status = append(kernel_->loaded_gui_desktop_module() != nullptr ? "system" : "kernel"); !status.ok())
    {
        return status;
    }
    if (auto status = append(" apps="); !status.ok())
    {
        return status;
    }
    bool first = true;
    for (const auto service : {"gui.app.about", "gui.app.prefs", "gui.app.notes"})
    {
        const std::string_view service_id{service};
        if (!kernel_->kernel_modules().services().contains(service_id))
        {
            continue;
        }
        if (!first)
        {
            if (auto status = append(","); !status.ok())
            {
                return status;
            }
        }
        if (auto status = append(service_id == "gui.app.about"    ? "about"
                                 : service_id == "gui.app.prefs" ? "prefs"
                                                                 : "notes");
            !status.ok())
        {
            return status;
        }
        first = false;
    }
    if (first)
    {
        if (auto status = append("none"); !status.ok())
        {
            return status;
        }
    }
    if (auto status = append("\ncommands: fm tui / | top | status | users\n"); !status.ok())
    {
        return status;
    }
    return Status::success();
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

Status KernelDebugShell::command_processes(std::string_view)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }

    const auto active = kernel_->posix().user_credentials();
    if (auto status = append("  PID USER    TTY   STAT THR COMMAND\n"); !status.ok())
    {
        return status;
    }
    for (const auto &process : kernel_->scheduler().processes())
    {
        if (process.credentials().kernel_space && !active.kernel_space)
        {
            continue;
        }
        FixedString<4> state;
        if (auto status = state.append(process_state_label(process.state())); !status.ok())
        {
            return status;
        }
        if (process.pid() == kernel_->scheduler().current_pid())
        {
            if (auto status = state.append("+"); !status.ok())
            {
                return status;
            }
        }
        if (process.background())
        {
            if (auto status = state.append("B"); !status.ok())
            {
                return status;
            }
        }

        if (auto status = append_padded_unsigned(process.pid(), 5); !status.ok())
        {
            return status;
        }
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
        std::string_view user_label = "user";
        if (process.credentials().kernel_space)
        {
            user_label = "kernel";
        }
        else if (const auto *account = kernel_->user_space().users().find_by_uid(process.credentials().euid);
                 account != nullptr)
        {
            user_label = account->name.view();
        }
        if (auto status = append_padded(user_label, 7); !status.ok())
        {
            return status;
        }
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
        if (auto status = append_padded(process_tty_label(process), 5); !status.ok())
        {
            return status;
        }
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
        if (auto status = append_padded(state.view(), 4); !status.ok())
        {
            return status;
        }
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
        if (auto status = append_padded_unsigned(process.threads().size(), 3); !status.ok())
        {
            return status;
        }
        if (auto status = append(" "); !status.ok())
        {
            return status;
        }
        if (auto status = append(process.name()); !status.ok())
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

Status KernelDebugShell::command_kill(std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }

    u64 pid = 0;
    if (!parse_unsigned(args, pid) || pid == 0)
    {
        return Status::invalid_argument("kill requires a numeric pid");
    }
    auto *process = kernel_->scheduler().find(static_cast<sched::ProcessId>(pid));
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    if (process->name() == "idle")
    {
        return Status::denied("idle process is protected");
    }
    const auto active = kernel_->posix().user_credentials();
    if (process->credentials().kernel_space && !active.kernel_space)
    {
        return Status::denied("only kernel debug user can kill kernel processes");
    }
    return kernel_->kill_process(static_cast<sched::ProcessId>(pid));
}

Status KernelDebugShell::command_power(SystemPowerAction action, std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto active = kernel_->posix().user_credentials();
    if (!active.kernel_space)
    {
        return Status::denied("only kernel debug user can change system power state");
    }
    args = trim_ascii(args);
    if (action == SystemPowerAction::poweroff && (args == "-r" || args == "-r now"))
    {
        action = SystemPowerAction::reboot;
    }
    else if (action == SystemPowerAction::poweroff && (args == "-h" || args == "-h now"))
    {
        args = {};
    }
    if (!args.empty() && args != "now")
    {
        return Status::unsupported("power command only accepts optional 'now', '-h', '-h now', '-r', or '-r now'");
    }
    if (auto status = kernel_->request_power_action(action); !status.ok())
    {
        return status;
    }

    switch (action)
    {
    case SystemPowerAction::halt:
        return append("system halt requested\n");
    case SystemPowerAction::poweroff:
        return append("system poweroff requested\n");
    case SystemPowerAction::reboot:
        return append("system reboot requested\n");
    case SystemPowerAction::none:
        break;
    }
    return Status::success();
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
    if (auto status = append(" uid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->posix().getuid()); !status.ok())
    {
        return status;
    }
    if (auto status = append(" euid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kernel_->posix().geteuid()); !status.ok())
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

Status KernelDebugShell::command_history()
{
    for (usize i = 0; i < gui_input_history_count_; ++i)
    {
        if (auto status = append_padded_unsigned(i + 1, 4); !status.ok())
        {
            return status;
        }
        if (auto status = append("  "); !status.ok())
        {
            return status;
        }
        if (auto status = append(gui_input_history_[i].view()); !status.ok())
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

Status KernelDebugShell::command_env()
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    if (auto status = append("PWD="); !status.ok())
    {
        return status;
    }
    if (auto status = append(kernel_->posix().getcwd()); !status.ok())
    {
        return status;
    }
    if (auto status = append("\nUSER="); !status.ok())
    {
        return status;
    }
    if (auto status = append(session_user_name_.view()); !status.ok())
    {
        return status;
    }
    if (auto status = append("\n"); !status.ok())
    {
        return status;
    }
    for (const auto &entry : environment_)
    {
        if (auto status = append(entry.name.view()); !status.ok())
        {
            return status;
        }
        if (auto status = append("="); !status.ok())
        {
            return status;
        }
        if (auto status = append(entry.value.view()); !status.ok())
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

Status KernelDebugShell::command_export(std::string_view args)
{
    args = trim_ascii(args);
    if (args.empty())
    {
        return command_env();
    }
    usize equals = args.size();
    for (usize i = 0; i < args.size(); ++i)
    {
        if (args[i] == '=')
        {
            equals = i;
            break;
        }
    }
    if (equals == args.size())
    {
        return set_environment_variable(shell_detail::first_word(args), {});
    }
    return set_environment_variable(trim_ascii(args.substr(0, equals)), args.substr(equals + 1));
}

Status KernelDebugShell::command_unset(std::string_view args)
{
    const auto name = shell_detail::first_word(args);
    if (name.empty())
    {
        return Status::invalid_argument("unset requires a variable name");
    }
    for (usize i = 0; i < environment_.size(); ++i)
    {
        if (environment_[i].name.view() == name)
        {
            return environment_.erase_at(i);
        }
    }
    return Status::success();
}

Status KernelDebugShell::command_type(std::string_view args)
{
    const auto command = shell_detail::first_word(args);
    if (command.empty())
    {
        return Status::invalid_argument("type requires a command");
    }
    if (!known_shell_command(command))
    {
        return Status::not_found("command not found");
    }
    if (auto status = append(command); !status.ok())
    {
        return status;
    }
    return append(" is a shell builtin\n");
}

Status KernelDebugShell::command_which(std::string_view args)
{
    const auto command = shell_detail::first_word(args);
    if (command.empty())
    {
        return Status::invalid_argument("which requires a command");
    }
    if (!known_shell_command(command))
    {
        return Status::not_found("command not found");
    }
    if (command == "fm" || command == "fileman" || command == "taskman" || command == "top")
    {
        if (auto status = append("/kernel/apps/"); !status.ok())
        {
            return status;
        }
    }
    else if (auto status = append("/kernel/bin/"); !status.ok())
    {
        return status;
    }
    if (auto status = append(command); !status.ok())
    {
        return status;
    }
    return append("\n");
}

} // namespace ok
