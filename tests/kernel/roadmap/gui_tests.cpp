#include "roadmap_tests.hpp"

#include "ok/core/entry.hpp"
#include "ok/gui/gui.hpp"

namespace ok
{
namespace
{

bool file_contains(const fs::FileBuffer &file, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (needle.size() > file.size)
    {
        return false;
    }
    for (usize i = 0; i + needle.size() <= file.size; ++i)
    {
        bool match = true;
        for (usize j = 0; j < needle.size(); ++j)
        {
            if (std::to_integer<char>(file.data[i + j]) != needle[j])
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }
    return false;
}

Status append_unsigned(FixedString<32> &out, u64 value)
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
            if (auto status = out.append(static_cast<char>('0' + digit)); !status.ok())
            {
                return status;
            }
            started = true;
        }
    }
    return Status::success();
}

Status release_shell_surface(Kernel &kernel)
{
    const auto id = kernel.debug_shell().gui_surface_id();
    if (id == 0 || !kernel.gui().compositor().surface_info(id))
    {
        return Status::success();
    }
    return kernel.gui().compositor().destroy_surface(id);
}

Status test_gui_compositor_draws_surfaces(Kernel &kernel)
{
    if (auto status = release_shell_surface(kernel); !status.ok())
    {
        return status;
    }
    auto &compositor = kernel.gui().compositor();
    const auto before = kernel.display().checksum();
    auto surface = compositor.create_surface(gui::Rect{.x = 4, .y = 5, .width = 24, .height = 12}, "panel");
    if (!surface)
    {
        return surface.status();
    }
    if (auto status = compositor.fill(surface.value(), 0xff223344u); !status.ok())
    {
        return status;
    }
    if (auto status =
            compositor.fill_rect(surface.value(), gui::Rect{.x = -4, .y = 3, .width = 14, .height = 5}, 0xff66cc88u);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.put_pixel(surface.value(), 7, 7, 0xffffcc66u); !status.ok())
    {
        return status;
    }
    auto info = compositor.surface_info(surface.value());
    if (!info || info.value().bounds.width != 24 || info.value().title != "panel")
    {
        return Status::fault("GUI surface metadata validation failed");
    }
    if (auto status = compositor.present(); !status.ok())
    {
        return status;
    }
    if (compositor.last_present_checksum() == 0 || compositor.last_present_checksum() == before)
    {
        return Status::fault("GUI compositor did not update the framebuffer");
    }
    return compositor.destroy_surface(surface.value());
}

Status test_gui_text_uses_bitmap_font(Kernel &kernel)
{
    auto &compositor = kernel.gui().compositor();
    constexpr i32 surface_left = 10;
    constexpr i32 surface_top = 10;
    constexpr u32 foreground = 0xffabcdefu;
    constexpr u32 background = 0xff010203u;
    auto surface = compositor.create_surface(gui::Rect{.x = surface_left, .y = surface_top, .width = 20, .height = 18},
                                             "text");
    if (!surface)
    {
        return surface.status();
    }
    if (auto status = compositor.fill(surface.value(), background); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.draw_text(surface.value(), 1, 1, "A", foreground, background); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.present(); !status.ok())
    {
        return status;
    }

    const u32 glyph_left = static_cast<u32>(surface_left) + gui::gui_glyph_width;
    const u32 glyph_top = static_cast<u32>(surface_top) + gui::gui_glyph_height;
    auto top_left = kernel.display().pixel_at(glyph_left, glyph_top);
    auto top_bar = kernel.display().pixel_at(glyph_left + 1, glyph_top);
    auto crossbar_left = kernel.display().pixel_at(glyph_left, glyph_top + 3);
    if (!top_left)
    {
        return top_left.status();
    }
    if (!top_bar)
    {
        return top_bar.status();
    }
    if (!crossbar_left)
    {
        return crossbar_left.status();
    }
    if (top_left.value() != background || top_bar.value() != foreground || crossbar_left.value() != foreground)
    {
        return Status::fault("GUI text did not use bitmap font rows");
    }
    return compositor.destroy_surface(surface.value());
}

Status test_gui_surface_management_api(Kernel &kernel)
{
    if (auto status = release_shell_surface(kernel); !status.ok())
    {
        return status;
    }
    auto &compositor = kernel.gui().compositor();
    auto desktop = compositor.desktop_bounds();
    if (!desktop || desktop.value().width != driver::framebuffer_width ||
        desktop.value().height != driver::framebuffer_height)
    {
        return Status::fault("GUI desktop bounds query failed");
    }

    auto back = compositor.create_surface(gui::Rect{.x = 4, .y = 5, .width = 24, .height = 14}, "back");
    auto front = compositor.create_surface(gui::Rect{.x = 8, .y = 8, .width = 18, .height = 12}, "front");
    if (!back || !front)
    {
        return !back ? back.status() : front.status();
    }
    if (auto status = compositor.fill(back.value(), 0xff223344u); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.fill(front.value(), 0xff556677u); !status.ok())
    {
        return status;
    }
    auto top = compositor.surface_at(9, 9);
    auto front_info = compositor.surface_info(front.value());
    if (!top || top.value() != front.value() || compositor.active_surface() != front.value() || !front_info ||
        !front_info.value().focused)
    {
        return Status::fault("GUI surface hit-test did not select top surface");
    }
    if (auto status = compositor.raise_surface(back.value()); !status.ok())
    {
        return status;
    }
    top = compositor.surface_at(9, 9);
    auto back_info = compositor.surface_info(back.value());
    if (!top || top.value() != back.value() || compositor.active_surface() != back.value() || !back_info ||
        !back_info.value().focused)
    {
        return Status::fault("GUI raise surface did not update z order and focus");
    }
    if (auto status = compositor.set_visible(back.value(), false); !status.ok())
    {
        return status;
    }
    top = compositor.surface_at(9, 9);
    if (!top || top.value() != front.value())
    {
        return Status::fault("GUI hidden surface still participated in hit-test");
    }
    if (auto status = compositor.set_visible(back.value(), true); !status.ok())
    {
        return status;
    }
    if (auto status =
            compositor.resize_surface(front.value(), gui::Rect{.x = 30, .y = 24, .width = 20, .height = 16});
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.move_surface(front.value(), 32, 26); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.set_title(front.value(), "front-renamed"); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.maximize_surface(front.value()); !status.ok())
    {
        return status;
    }
    auto info = compositor.surface_info(front.value());
    if (!info || info.value().window_state != gui::WindowState::maximized ||
        info.value().bounds.width != driver::framebuffer_width ||
        info.value().bounds.height != driver::framebuffer_height - gui::taskbar_height ||
        compositor.active_surface() != front.value())
    {
        return Status::fault("GUI maximize surface did not update window state");
    }
    if (auto status = compositor.restore_surface(front.value()); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.minimize_surface(front.value()); !status.ok())
    {
        return status;
    }
    info = compositor.surface_info(front.value());
    if (!info || !info.value().visible || info.value().window_state != gui::WindowState::minimized ||
        info.value().bounds.y < static_cast<i32>(driver::framebuffer_height - gui::taskbar_height) ||
        compositor.active_surface() != back.value())
    {
        return Status::fault("GUI minimize surface did not dock the window and move focus");
    }
    if (auto status = compositor.restore_surface(front.value()); !status.ok())
    {
        return status;
    }
    info = compositor.surface_info(front.value());
    if (!info || info.value().bounds.x != 32 || info.value().bounds.y != 26 ||
        info.value().bounds.width != 20 || info.value().bounds.height != 16 ||
        info.value().title != "front-renamed" || info.value().window_state != gui::WindowState::normal ||
        compositor.active_surface() != front.value())
    {
        return Status::fault("GUI surface metadata update failed");
    }
    if (auto status = compositor.present(); !status.ok())
    {
        return status;
    }
    if (compositor.last_present_checksum() == 0)
    {
        return Status::fault("GUI managed surfaces did not present");
    }
    if (auto status = compositor.close_surface(front.value()); !status.ok())
    {
        return status;
    }
    return compositor.destroy_surface(back.value());
}

Status test_gui_mouse_interacts_with_windows(Kernel &kernel)
{
    if (auto status = release_shell_surface(kernel); !status.ok())
    {
        return status;
    }
    auto &compositor = kernel.gui().compositor();
    auto surface = compositor.create_surface(gui::Rect{.x = 60, .y = 60, .width = 80, .height = 50}, "mouse");
    if (!surface)
    {
        return surface.status();
    }
    if (auto status = compositor.fill(surface.value(), 0xff1c2f38u); !status.ok())
    {
        return status;
    }

    if (auto status = compositor.set_pointer_position(70, 66); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(20, 16, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, false); !status.ok())
    {
        return status;
    }
    auto info = compositor.surface_info(surface.value());
    if (!info || info.value().bounds.x != 80 || info.value().bounds.y != 76)
    {
        return Status::fault("GUI mouse drag did not move the window");
    }

    if (auto status =
            compositor.set_pointer_position(info.value().bounds.x + static_cast<i32>(info.value().bounds.width) - 2,
                                            info.value().bounds.y + static_cast<i32>(info.value().bounds.height) - 2);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(24, 20, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, false); !status.ok())
    {
        return status;
    }
    info = compositor.surface_info(surface.value());
    if (!info || info.value().bounds.width <= 80 || info.value().bounds.height <= 50)
    {
        return Status::fault("GUI mouse resize did not resize the window");
    }

    if (auto status =
            compositor.set_pointer_position(info.value().bounds.x + static_cast<i32>(info.value().bounds.width) - 20,
                                            info.value().bounds.y + 5);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, false); !status.ok())
    {
        return status;
    }
    info = compositor.surface_info(surface.value());
    if (!info || info.value().window_state != gui::WindowState::maximized ||
        info.value().bounds.height != driver::framebuffer_height - gui::taskbar_height)
    {
        return Status::fault("GUI mouse maximize button did not maximize the window");
    }
    if (auto status =
            compositor.set_pointer_position(info.value().bounds.x + static_cast<i32>(info.value().bounds.width) - 28,
                                            info.value().bounds.y + 5);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, false); !status.ok())
    {
        return status;
    }
    info = compositor.surface_info(surface.value());
    if (!info || info.value().window_state != gui::WindowState::normal)
    {
        return Status::fault("GUI mouse minimize button did not restore a maximized window");
    }
    if (auto status =
            compositor.set_pointer_position(info.value().bounds.x + static_cast<i32>(info.value().bounds.width) - 20,
                                            info.value().bounds.y + 5);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, false); !status.ok())
    {
        return status;
    }
    info = compositor.surface_info(surface.value());
    if (!info || info.value().window_state != gui::WindowState::maximized)
    {
        return Status::fault("GUI mouse maximize button did not maximize after restore");
    }
    if (auto status =
            compositor.set_pointer_position(info.value().bounds.x + static_cast<i32>(info.value().bounds.width) - 20,
                                            info.value().bounds.y + 5);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, false); !status.ok())
    {
        return status;
    }
    info = compositor.surface_info(surface.value());
    if (!info || info.value().window_state != gui::WindowState::normal)
    {
        return Status::fault("GUI mouse maximize button did not restore the window");
    }
    if (auto status =
            compositor.set_pointer_position(info.value().bounds.x + static_cast<i32>(info.value().bounds.width) - 28,
                                            info.value().bounds.y + 5);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, false); !status.ok())
    {
        return status;
    }
    info = compositor.surface_info(surface.value());
    if (!info || info.value().window_state != gui::WindowState::minimized)
    {
        return Status::fault("GUI mouse minimize button did not minimize the window");
    }
    if (auto status = compositor.set_pointer_position(info.value().bounds.x + 8, info.value().bounds.y + 5);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, false); !status.ok())
    {
        return status;
    }
    info = compositor.surface_info(surface.value());
    if (!info || info.value().window_state != gui::WindowState::normal)
    {
        return Status::fault("GUI mouse minimized task button did not restore the window");
    }

    if (auto status =
            compositor.set_pointer_position(info.value().bounds.x + static_cast<i32>(info.value().bounds.width) - 10,
                                            info.value().bounds.y + 5);
        !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.handle_mouse_delta(0, 0, false); !status.ok())
    {
        return status;
    }
    const auto close_event = compositor.consume_window_event();
    if (close_event.kind != gui::WindowEventKind::close_request || close_event.surface_id != surface.value())
    {
        return Status::fault("GUI mouse close button did not emit a close request");
    }
    if (auto status = compositor.close_surface(surface.value()); !status.ok())
    {
        return status;
    }
    if (compositor.surface_info(surface.value()))
    {
        return Status::fault("GUI mouse close button did not close the window");
    }
    return Status::success();
}

Status test_gui_taskbar_launchers_and_focused_keyboard(Kernel &kernel)
{
    if (auto status = kernel.debug_shell().close_all_gui(); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.close_file_manager(); !status.ok())
    {
        return status;
    }

    auto &compositor = kernel.gui().compositor();
    const auto launcher_y = static_cast<i32>(driver::framebuffer_height - gui::taskbar_height + 3 +
                                             gui::taskbar_icon_size / 2);
    const auto shell_x = static_cast<i32>(6 + gui::taskbar_icon_size / 2);
    auto launcher = compositor.taskbar_launcher_at(shell_x, launcher_y);
    if (!launcher || launcher.value() != gui::TaskbarApp::debug_shell)
    {
        return Status::fault("GUI taskbar shell launcher hit-test failed");
    }
    if (auto status = compositor.set_pointer_position(shell_x, launcher_y); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, false); !status.ok())
    {
        return status;
    }
    const auto shell_surface = kernel.debug_shell().gui_surface_id();
    auto shell_info = compositor.surface_info(shell_surface);
    if (shell_surface == 0 || !shell_info || shell_info.value().app != gui::TaskbarApp::debug_shell ||
        !shell_info.value().focused)
    {
        return Status::fault("GUI taskbar shell launcher did not open and focus oksh");
    }
    const auto first_shell_pid = kernel.debug_shell().process_id();
    const auto surface_count = compositor.surface_count();
    if (auto status = kernel.handle_gui_mouse(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, false); !status.ok())
    {
        return status;
    }
    if (compositor.surface_count() <= surface_count || kernel.debug_shell().gui_surface_id() == shell_surface ||
        kernel.debug_shell().process_id() == first_shell_pid ||
        kernel.scheduler().find(first_shell_pid) == nullptr ||
        kernel.scheduler().find(kernel.debug_shell().process_id()) == nullptr)
    {
        return Status::fault("GUI taskbar shell launcher did not create another oksh window");
    }

    const auto files_x = static_cast<i32>(6 + gui::taskbar_icon_size + 6 + gui::taskbar_icon_size / 2);
    launcher = compositor.taskbar_launcher_at(files_x, launcher_y);
    if (!launcher || launcher.value() != gui::TaskbarApp::file_manager)
    {
        return Status::fault("GUI taskbar file manager launcher hit-test failed");
    }
    if (auto status = compositor.set_pointer_position(files_x, launcher_y); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, false); !status.ok())
    {
        return status;
    }
    auto file_info = compositor.surface_info(kernel.file_manager().surface_id());
    if (kernel.file_manager().surface_id() == 0 || !file_info ||
        file_info.value().app != gui::TaskbarApp::file_manager || !file_info.value().focused)
    {
        return Status::fault("GUI taskbar file manager launcher did not open and focus fm");
    }

    if (auto status = kernel.handle_gui_key('x'); !status.ok())
    {
        return status;
    }
    if (!kernel.debug_shell().gui_input_line().empty())
    {
        return Status::fault("GUI keyboard input leaked into unfocused shell");
    }
    if (auto status = compositor.raise_surface(shell_surface); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_key('p'); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_key('s'); !status.ok())
    {
        return status;
    }
    if (kernel.debug_shell().gui_input_line() != "ps")
    {
        return Status::fault("GUI focused shell did not receive keyboard input");
    }
    return Status::success();
}

Status test_gui_module_restarts_after_crash(Kernel &kernel)
{
    auto &module = kernel.gui();
    auto &manager = kernel.kernel_modules();
    if (manager.services().query<gui::GuiModule>(gui::gui_service_id) != &module ||
        module.compositor().state() != gui::GuiState::running)
    {
        return Status::fault("GUI module service publication failed");
    }

    const auto first_generation = module.compositor().generation();
    auto surface = module.compositor().create_surface(gui::Rect{.x = 1, .y = 1, .width = 10, .height = 8}, "live");
    if (!surface)
    {
        return surface.status();
    }
    if (auto status = module.compositor().simulate_crash("roadmap fault"); !status.ok())
    {
        return status;
    }
    if (module.compositor().create_surface(gui::Rect{.x = 0, .y = 0, .width = 4, .height = 4}, "bad").status().code() !=
        StatusCode::fault)
    {
        return Status::fault("crashed GUI compositor accepted new work");
    }

    gui::GuiSupervisor supervisor{manager, module};
    if (auto status = supervisor.tick(); !status.ok())
    {
        return status;
    }
    if (supervisor.restart_attempts() != 1 || module.state() != ModuleState::started ||
        module.compositor().state() != gui::GuiState::running || module.compositor().generation() <= first_generation ||
        module.compositor().surface_count() != 0 || manager.started_count() == 0 ||
        manager.services().query<gui::GuiModule>(gui::gui_service_id) != &module)
    {
        return Status::fault("GUI module restart validation failed");
    }

    auto recovered = module.compositor().create_surface(gui::Rect{.x = 3, .y = 4, .width = 16, .height = 10}, "new");
    if (!recovered)
    {
        return recovered.status();
    }
    if (auto status = module.compositor().fill(recovered.value(), 0xff4477aau); !status.ok())
    {
        return status;
    }
    if (auto status = module.compositor().present(); !status.ok())
    {
        return status;
    }
    if (module.compositor().last_present_checksum() == 0)
    {
        return Status::fault("restarted GUI did not present");
    }
    return module.compositor().destroy_surface(recovered.value());
}

Status test_gui_module_daemon_kill_restarts(Kernel &kernel)
{
    static_cast<void>(kernel.debug_shell().execute("su kernel"));
    const auto previous_pid = kernel.kernel_modules().kernel_process_pid();
    const auto previous_generation = kernel.gui().compositor().generation();
    if (previous_pid == 0 || kernel.scheduler().find(previous_pid) == nullptr)
    {
        return Status::fault("GUI module daemon process missing before kill restart test");
    }

    FixedString<32> kill_command;
    if (auto status = kill_command.assign("kill "); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kill_command, previous_pid); !status.ok())
    {
        return status;
    }
    auto killed = kernel.debug_shell().execute(kill_command.view());
    const auto restarted_pid = kernel.kernel_modules().kernel_process_pid();
    const auto *process = kernel.scheduler().find(restarted_pid);
    auto log = kernel.vfs().read_file("/tmp/kernel.log");
    if (!killed || !killed.value().empty() || kernel.scheduler().find(previous_pid) != nullptr ||
        restarted_pid == 0 || restarted_pid == previous_pid || process == nullptr ||
        process->name() != "mod:kernel-gui" || kernel.gui().compositor().state() != gui::GuiState::running ||
        kernel.gui().compositor().generation() <= previous_generation || !log ||
        !file_contains(log.value(), "module: restarted mod:kernel-gui"))
    {
        return Status::fault("kernel user kill did not restart and log the GUI module daemon");
    }
    return Status::success();
}

Status test_kernel_gui_is_started(Kernel &kernel)
{
    auto &module = kernel.gui();
    const auto *process = kernel.scheduler().find(kernel.kernel_modules().kernel_process_pid());
    if (module.state() != ModuleState::started || module.compositor().state() != gui::GuiState::running ||
        module.compositor().last_present_checksum() == 0 ||
        module.compositor().startup_animation_frames() < 8 ||
        kernel.kernel_modules().services().query<gui::GuiModule>(gui::gui_service_id) != &module ||
        module.manifest().execution != ModuleExecution::kernel_process ||
        kernel.kernel_modules().kernel_process_pid() == 0 ||
        process == nullptr || process->name() != "mod:kernel-gui" ||
        kernel.kernel_modules().kernel_process_module_count() == 0)
    {
        return Status::fault("kernel GUI module was not started during boot");
    }
    return Status::success();
}

Status test_kernel_file_manager_draws_vfs(Kernel &kernel)
{
    auto &manager = kernel.file_manager();
    auto &compositor = kernel.gui().compositor();
    const auto saved_credentials = kernel.posix().user_credentials();
    if (auto status = kernel.posix().set_credentials(user::kernel_credentials()); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.open_file_manager("/"); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    const auto *process = kernel.scheduler().find(manager.process_id());
    auto info = compositor.surface_info(manager.surface_id());
    if (manager.surface_id() == 0 || manager.path() != "/" || manager.render_count() == 0 || !info ||
        compositor.last_present_checksum() == 0)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager did not render the VFS root");
    }
    if (process == nullptr)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager process was not registered");
    }
    if (process->name() != "fm:kernel")
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager process was not named for kernel user");
    }
    if (!process->credentials().kernel_space)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager process did not keep kernel credentials");
    }
    const auto resize_render_count = manager.render_count();
    if (auto status =
            compositor.set_pointer_position(info.value().bounds.x + static_cast<i32>(info.value().bounds.width) - 20,
                                            info.value().bounds.y + 5);
        !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, true); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, false); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    info = compositor.surface_info(manager.surface_id());
    if (!info || info.value().window_state != gui::WindowState::maximized ||
        info.value().bounds.width != driver::framebuffer_width ||
        info.value().bounds.height != driver::framebuffer_height - gui::taskbar_height ||
        manager.render_count() <= resize_render_count)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager maximize did not resize and redraw");
    }
    const auto before_render_count = manager.render_count();
    const auto nav_x = info.value().bounds.x + 12;
    const auto nav_y = info.value().bounds.y + static_cast<i32>(gui::gui_glyph_height * 6 + 2);
    if (auto status = compositor.set_pointer_position(nav_x, nav_y); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, true); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, false); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (manager.path() != "/tmp" || manager.render_count() <= before_render_count)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager mouse navigation did not open /tmp");
    }
    const auto before_parent_render_count = manager.render_count();
    const auto parent_x = info.value().bounds.x + 80;
    const auto parent_y = info.value().bounds.y + static_cast<i32>(gui::gui_glyph_height * 6 + 2);
    if (auto status = compositor.set_pointer_position(parent_x, parent_y); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, true); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, false); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (manager.path() != "/" || manager.render_count() <= before_parent_render_count)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager parent navigation did not return to /");
    }
    const auto manager_pid = manager.process_id();
    const auto manager_surface = manager.surface_id();
    FixedString<32> kill_command;
    if (auto status = kill_command.assign("kill "); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = append_unsigned(kill_command, manager_pid); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    auto killed = kernel.debug_shell().execute(kill_command.view());
    const auto *killed_process = kernel.scheduler().find(manager_pid);
    if (!killed || !killed.value().empty() || killed_process != nullptr || manager.surface_id() != 0 ||
        manager.process_id() != 0 || compositor.surface_info(manager_surface))
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("debug shell kill did not close the GUI file manager process");
    }
    if (auto status = kernel.open_file_manager("/"); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    info = compositor.surface_info(manager.surface_id());
    const auto close_pid = manager.process_id();
    if (!info || close_pid == 0)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager did not reopen for close button test");
    }
    if (auto status =
            compositor.set_pointer_position(info.value().bounds.x + static_cast<i32>(info.value().bounds.width) - 10,
                                            info.value().bounds.y + 5);
        !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, true); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, false); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (manager.surface_id() != 0 || manager.process_id() != 0 || kernel.scheduler().find(close_pid) != nullptr)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager close button did not notify and close its process");
    }
    if (auto status = kernel.close_file_manager(); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    return kernel.posix().set_credentials(saved_credentials);
}

Status test_shell_renders_to_gui(Kernel &kernel)
{
    if (auto status = kernel.debug_shell().show_gui(); !status.ok())
    {
        return status;
    }
    const auto shell_pid = kernel.debug_shell().process_id();
    const auto *shell_process = kernel.scheduler().find(shell_pid);
    if (shell_pid == 0 || shell_process == nullptr || shell_process->name() != "oksh" ||
        shell_process->state() == sched::ProcessState::exited)
    {
        return Status::fault("GUI debug shell was not registered as a scheduler process");
    }
    const auto before_renders = kernel.debug_shell().gui_render_count();
    const auto before_checksum = kernel.gui().compositor().last_present_checksum();
    auto output = kernel.debug_shell().execute("echo gui-shell");
    if (!output || output.value() != "gui-shell\n")
    {
        return Status::fault("GUI shell command output validation failed");
    }
    if (kernel.debug_shell().gui_render_count() <= before_renders || kernel.debug_shell().gui_surface_id() == 0 ||
        kernel.gui().compositor().last_present_checksum() == 0 ||
        kernel.gui().compositor().last_present_checksum() == before_checksum)
    {
        return Status::fault("debug shell did not render to GUI");
    }
    auto surface = kernel.gui().compositor().surface_info(kernel.debug_shell().gui_surface_id());
    if (!surface || (surface.value().bounds.x == 0 && surface.value().bounds.y == 0 &&
                     surface.value().bounds.width == driver::framebuffer_width &&
                     surface.value().bounds.height == driver::framebuffer_height))
    {
        return Status::fault("debug shell GUI surface should start as a resizable window");
    }
    if (auto status =
            kernel.gui().compositor().set_pointer_position(surface.value().bounds.x +
                                                               static_cast<i32>(surface.value().bounds.width) - 20,
                                                           surface.value().bounds.y + 5);
        !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, true); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, false); !status.ok())
    {
        return status;
    }
    surface = kernel.gui().compositor().surface_info(kernel.debug_shell().gui_surface_id());
    if (!surface || surface.value().window_state != gui::WindowState::maximized ||
        surface.value().bounds.width != driver::framebuffer_width ||
        surface.value().bounds.height != driver::framebuffer_height - gui::taskbar_height)
    {
        return Status::fault("debug shell GUI maximize button did not resize and redraw");
    }
    const auto input_checksum = kernel.gui().compositor().last_present_checksum();
    if (auto status = kernel.debug_shell().set_gui_input("ps"); !status.ok())
    {
        return status;
    }
    if (kernel.gui().compositor().last_present_checksum() == input_checksum)
    {
        return Status::fault("debug shell GUI input line did not redraw");
    }
    const auto first_shell_surface = kernel.debug_shell().gui_surface_id();
    const auto first_shell_pid = kernel.debug_shell().process_id();
    const auto surface_count_before_second_shell = kernel.gui().compositor().surface_count();
    if (auto status = kernel.handle_gui_key(ok_input_open_shell); !status.ok())
    {
        return status;
    }
    if (kernel.debug_shell().gui_surface_id() == first_shell_surface ||
        kernel.debug_shell().process_id() == first_shell_pid ||
        kernel.gui().compositor().surface_count() <= surface_count_before_second_shell ||
        kernel.scheduler().find(first_shell_pid) == nullptr ||
        kernel.scheduler().find(kernel.debug_shell().process_id()) == nullptr)
    {
        return Status::fault("F12 did not create a new managed debug shell window");
    }
    for (usize i = 0; i < 32; ++i)
    {
        auto scroll_output = kernel.debug_shell().execute("echo gui-scroll");
        if (!scroll_output || scroll_output.value() != "gui-scroll\n")
        {
            return Status::fault("debug shell scroll history setup failed");
        }
    }
    const auto bottom_checksum = kernel.gui().compositor().last_present_checksum();
    if (auto status = kernel.debug_shell().scroll_gui_history(1); !status.ok())
    {
        return status;
    }
    const auto scrolled_checksum = kernel.gui().compositor().last_present_checksum();
    if (scrolled_checksum == bottom_checksum)
    {
        return Status::fault("debug shell GUI scrollback did not redraw");
    }
    if (auto status = kernel.debug_shell().scroll_gui_history(-1); !status.ok())
    {
        return status;
    }
    if (kernel.gui().compositor().last_present_checksum() == scrolled_checksum)
    {
        return Status::fault("debug shell GUI scrollback did not return to prompt");
    }
    auto clear_output = kernel.debug_shell().execute("clear");
    bool prompt_visible = false;
    for (u32 y = gui::gui_glyph_height * 3; y < gui::gui_glyph_height * 4; ++y)
    {
        for (u32 x = gui::gui_glyph_width; x < gui::gui_glyph_width * 5; ++x)
        {
            auto pixel = kernel.display().pixel_at(x, y);
            if (!pixel)
            {
                return pixel.status();
            }
            if (pixel.value() != 0xff061018u)
            {
                prompt_visible = true;
            }
        }
    }
    if (!clear_output || clear_output.value() != "\f" || !prompt_visible)
    {
        return Status::fault("debug shell GUI clear did not redraw the prompt");
    }
    return Status::success();
}

} // namespace

Status run_gui_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    if (auto status = test_gui_compositor_draws_surfaces(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_gui_text_uses_bitmap_font(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_gui_surface_management_api(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_gui_mouse_interacts_with_windows(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_gui_taskbar_launchers_and_focused_keyboard(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_gui_module_restarts_after_crash(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_gui_module_daemon_kill_restarts(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_kernel_gui_is_started(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_kernel_file_manager_draws_vfs(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_shell_renders_to_gui(kernel); !status.ok())
    {
        return status;
    }

    report.gui = true;
    return Status::success();
}

} // namespace ok
