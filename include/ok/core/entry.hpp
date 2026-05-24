#pragma once

#include "ok/core/kernel.hpp"

namespace ok
{

using KernelDebugWrite = void (*)(void *context, std::string_view text);

struct KernelDebugSink
{
    void *context{};
    KernelDebugWrite write{};

    void emit(std::string_view text) const
    {
        if (write != nullptr)
        {
            write(context, text);
        }
    }
};

enum class KernelBootMode : u8
{
    normal,
    debug,
};

struct KernelEntryConfig
{
    KernelBootMode mode{KernelBootMode::normal};
    KernelConfig kernel{};
    KernelDebugSink debug{};
};

struct KernelEntryResult
{
    Status status{};
    KernelTestReport tests{};
    usize debug_test_points{};
    u64 display_checksum{};
};

Status ok_kernel_entry(const KernelEntryConfig &config, KernelEntryResult *result = nullptr);
Result<std::string_view> ok_debug_shell_execute(std::string_view command);
Status ok_debug_shell_show_gui();
Status ok_debug_shell_close_gui();
Status ok_debug_shell_set_gui_input(std::string_view line);
Status ok_debug_shell_scroll_gui(i32 rows);
Status ok_debug_shell_open_file_manager_shortcut();
bool ok_debug_shell_has_foreground_process();
Status ok_debug_shell_interrupt();
Status ok_gui_mouse_event(i32 delta_x, i32 delta_y, bool left_button);
Status ok_gui_key_event(int key);
Status ok_gui_close_debug_surfaces();
bool ok_debug_shell_gui_ready();
bool ok_debug_shell_gui_open();

inline constexpr int ok_input_open_shell = 0x1001;
inline constexpr int ok_input_open_file_manager = 0x1002;

extern "C" int ok_kernel_main(const KernelEntryConfig *config);

} // namespace ok
