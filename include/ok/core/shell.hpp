#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/power.hpp"
#include "ok/core/types.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/gui/gui.hpp"
#include "ok/user/user.hpp"

#include <array>
#include <string_view>

namespace ok
{

class Kernel;

class KernelDebugShell final
{
  public:
    Status attach(Kernel &kernel);
    Result<std::string_view> execute(std::string_view line);
    Status show_gui();
    Status show_or_focus_gui();
    Status close_gui();
    Status close_all_gui();
    Status set_gui_input(std::string_view line);
    Status handle_key(int key);
    Status handle_key(gui::SurfaceId surface, int key);
    Status scroll_gui_history(i32 rows);
    void mark_gui_closed();
    Status reconcile_gui_windows();
    void notify_process_exit(sched::ProcessId pid);
    Status close_process_window(sched::ProcessId pid);
    Status close_surface_window(gui::SurfaceId surface);
    Status handle_surface_changed(gui::SurfaceId surface);
    Status start_foreground_process(sched::ProcessId pid);
    Status interrupt_foreground_process();
    [[nodiscard]] bool owns_process(sched::ProcessId pid) const;
    [[nodiscard]] bool owns_surface(gui::SurfaceId surface) const;
    [[nodiscard]] bool gui_ready();
    [[nodiscard]] bool gui_open() const
    {
        return gui_open_;
    }
    [[nodiscard]] sched::ProcessId process_id() const
    {
        return process_id_;
    }
    [[nodiscard]] sched::ProcessId foreground_process_id() const
    {
        return foreground_process_id_;
    }
    [[nodiscard]] usize gui_render_count() const
    {
        return gui_render_count_;
    }
    [[nodiscard]] gui::SurfaceId gui_surface_id() const
    {
        return gui_surface_id_;
    }
    [[nodiscard]] std::string_view gui_input_line() const
    {
        return gui_input_line_.view();
    }

  private:
    Status append(std::string_view text);
    Status append_unsigned(u64 value);
    Status append_padded(std::string_view text, usize width);
    Status append_padded_unsigned(u64 value, usize width);
    Status append_node_type(fs::NodeType type);
    Status append_session_user();
    Status render_to_gui(std::string_view command_line, std::string_view output);
    Status record_gui_window();
    Status select_gui_window(usize index);
    Status activate_gui_window(usize index);
    Status remove_gui_window(usize index);
    [[nodiscard]] Result<usize> find_window_by_process(sched::ProcessId pid) const;
    [[nodiscard]] Result<usize> find_window_by_surface(gui::SurfaceId surface) const;
    Status ensure_gui_process();
    Status ensure_gui_surface();
    Status sync_posix_credentials_to_session();
    Status append_gui_history(std::string_view text);
    Status append_gui_history_unsigned(u64 value);
    Status redraw_gui_terminal();
    Status refresh_process_credentials();
    Status remember_gui_input_line();
    Status recall_gui_history_previous();
    Status recall_gui_history_next();
    [[nodiscard]] bool foreground_process_running();
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
    Status command_processes(std::string_view args);
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
    Status command_chmod(std::string_view args);
    Status command_chown(std::string_view args);
    Status command_users();
    Status command_kill(std::string_view args);
    Status command_power(SystemPowerAction action, std::string_view args);
    Status command_whoami();
    Status command_id();
    Status command_su(std::string_view user);
    Status command_exit(std::string_view args);
    Status command_disk();
    Status command_mkfs(std::string_view label);
    Status command_simplefs(std::string_view args);
    Status command_ext4(std::string_view args);
    Status command_net(std::string_view args);
    Status command_file_manager(std::string_view path);

    struct GuiWindow
    {
        gui::SurfaceId surface_id{0};
        sched::ProcessId process_id{0};
    };

    Kernel *kernel_{nullptr};
    FixedString<32> session_user_name_{"kernel"};
    FixedString<32> previous_session_user_name_{};
    user::Credentials previous_credentials_{};
    bool has_previous_session_{false};
    FixedString<96> path_buffer_{};
    FixedString<4096> output_{};
    FixedString<2048> gui_history_{};
    FixedString<128> gui_input_line_{};
    gui::SurfaceId gui_surface_id_{0};
    sched::ProcessId process_id_{0};
    sched::ProcessId foreground_process_id_{0};
    StaticVector<GuiWindow, gui::max_gui_surfaces> gui_windows_{};
    std::array<FixedString<128>, 16> gui_input_history_{};
    usize gui_render_count_{0};
    usize gui_scroll_rows_{0};
    usize gui_input_history_count_{0};
    usize gui_input_history_cursor_{0};
    u8 gui_escape_state_{0};
    bool gui_open_{true};
};

} // namespace ok
