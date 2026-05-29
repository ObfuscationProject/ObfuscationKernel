#include "roadmap_tests.hpp"

#include "ok/core/entry.hpp"
#include "ok/driver/font.hpp"
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

std::span<const std::byte> bytes_for(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

constexpr std::string_view os_system_gui_module_path{"/boot/modules/system-gui.okmod"};
constexpr std::string_view os_system_gui_module_text{
    "OKMOD\n"
    "name=system-gui\n"
    "version=1\n"
    "vermagic=okernel-cxx-oop\n"
    "require=gui.compositor\n"
    "require=gui.desktop\n"
    "export=gui.system-desktop\n"
    "param=entry:oop\n"
    "param=class:desktop\n"
    "param=brand:ObfuscationOS\n"
    "param=title:ObfuscationOS Login\n"
    "param=subtitle:choose root or user\n"
    "signature=system-gui-dev\n"};

constexpr std::string_view os_about_app_module_path{"/boot/modules/apps/about.okmod"};
constexpr std::string_view os_about_app_module_flat_path{"apps_about.okmod"};
constexpr std::string_view os_about_app_module_text{
    "OKMOD\n"
    "name=system-about\n"
    "version=1\n"
    "vermagic=okernel-cxx-oop\n"
    "require=gui.compositor\n"
    "require=gui.desktop\n"
    "require=gui.system-desktop\n"
    "export=gui.app.about\n"
    "param=entry:oop\n"
    "param=class:app\n"
    "param=title:About ObfuscationOS\n"
    "param=subtitle:C++ OOP module from rootfs\n"
    "param=body:Loaded through OK_SYS_LOAD_MODULE\n"
    "param=x:54\n"
    "param=y:44\n"
    "param=accent:gold\n"
    "signature=system-about-dev\n"};

Status ensure_test_directory(Kernel &kernel, std::string_view path)
{
    auto stat = kernel.vfs().stat(path);
    if (stat)
    {
        return stat.value().type == fs::NodeType::directory ? Status::success()
                                                            : Status::invalid_argument("test path is not a directory");
    }
    if (stat.status().code() != StatusCode::not_found)
    {
        return stat.status();
    }
    return kernel.vfs().create(path, fs::NodeType::directory);
}

Status stage_os_system_gui_module(Kernel &kernel)
{
    if (auto status = ensure_test_directory(kernel, "/boot"); !status.ok())
    {
        return status;
    }
    if (auto status = ensure_test_directory(kernel, "/boot/modules"); !status.ok())
    {
        return status;
    }
    auto stat = kernel.vfs().stat(os_system_gui_module_path);
    if (!stat)
    {
        if (stat.status().code() != StatusCode::not_found)
        {
            return stat.status();
        }
        if (auto status = kernel.vfs().create(os_system_gui_module_path, fs::NodeType::regular); !status.ok())
        {
            return status;
        }
    }
    return kernel.vfs().write_file(os_system_gui_module_path, bytes_for(os_system_gui_module_text));
}

Result<gui::SurfaceInfo> find_surface_by_title(Kernel &kernel, std::string_view title)
{
    for (gui::SurfaceId id = 1; id < 64; ++id)
    {
        auto info = kernel.gui().compositor().surface_info(id);
        if (!info)
        {
            continue;
        }
        if (info.value().title == title)
        {
            return info.value();
        }
    }
    return Status::not_found("surface title was not found");
}

bool contains_text(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (needle.size() > haystack.size())
    {
        return false;
    }
    for (usize i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        bool match = true;
        for (usize j = 0; j < needle.size(); ++j)
        {
            if (haystack[i + j] != needle[j])
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

usize process_count_named(const sched::Scheduler &scheduler, std::string_view name)
{
    usize count = 0;
    for (const auto &process : scheduler.processes())
    {
        if (process.name() == name)
        {
            ++count;
        }
    }
    return count;
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

Status release_boot_gui_surfaces(Kernel &kernel)
{
    const auto id = kernel.debug_shell().gui_surface_id();
    if (id == 0 || !kernel.gui().compositor().surface_info(id))
    {
        auto dashboard = find_surface_by_title(kernel, "ObfuscationOS Login");
        if (dashboard)
        {
            return kernel.gui().compositor().destroy_surface(dashboard.value().id);
        }
        return Status::success();
    }
    if (auto status = kernel.gui().compositor().destroy_surface(id); !status.ok())
    {
        return status;
    }
    auto dashboard = find_surface_by_title(kernel, "ObfuscationOS Login");
    if (dashboard)
    {
        return kernel.gui().compositor().destroy_surface(dashboard.value().id);
    }
    return Status::success();
}

Status test_gui_compositor_draws_surfaces(Kernel &kernel)
{
    if (auto status = release_boot_gui_surfaces(kernel); !status.ok())
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
    bool percent_has_own_glyph = false;
    for (u32 row = 0; row < driver::BitmapFontRenderer::glyph_height; ++row)
    {
        if (driver::BitmapFontRenderer::glyph_row('%', row) != driver::BitmapFontRenderer::glyph_row('?', row))
        {
            percent_has_own_glyph = true;
            break;
        }
    }
    if (!percent_has_own_glyph)
    {
        return Status::fault("GUI bitmap font renders percent as the fallback glyph");
    }
    return compositor.destroy_surface(surface.value());
}

Status test_gui_surface_management_api(Kernel &kernel)
{
    if (auto status = release_boot_gui_surfaces(kernel); !status.ok())
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
    const auto taskbar_y = static_cast<i32>(driver::framebuffer_height - gui::taskbar_height + 5);
    const auto back_taskbar_x = static_cast<i32>(gui::taskbar_launcher_width + 12);
    auto taskbar_hit = compositor.surface_at(back_taskbar_x, taskbar_y);
    if (!taskbar_hit || taskbar_hit.value() != back.value())
    {
        return Status::fault("GUI taskbar did not expose an open normal window");
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
    if (auto status = release_boot_gui_surfaces(kernel); !status.ok())
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

Status test_kernel_gui_mouse_position_uses_absolute_pointer(Kernel &kernel)
{
    if (auto status = release_boot_gui_surfaces(kernel); !status.ok())
    {
        return status;
    }
    auto &compositor = kernel.gui().compositor();
    auto surface = compositor.create_surface(gui::Rect{.x = 100, .y = 40, .width = 96, .height = 48}, "position");
    if (!surface)
    {
        return surface.status();
    }
    if (auto status = compositor.fill(surface.value(), 0xff1c2f38u); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.set_pointer_position(0, 0); !status.ok())
    {
        return status;
    }
    const auto close_x = 100 + 96 - 10;
    const auto close_y = 40 + 5;
    if (auto status = kernel.handle_gui_mouse_position(close_x, close_y, true); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse_position(close_x, close_y, false); !status.ok())
    {
        return status;
    }
    if (compositor.surface_info(surface.value()))
    {
        return Status::fault("kernel GUI absolute mouse position did not hit the window close button");
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
    if (manager.services().query<gui::GuiCompositor>(gui::gui_service_id) != &module.compositor() ||
        manager.services().query<gui::GuiDesktopService>(gui::gui_desktop_service_id) != &module.desktop() ||
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
        manager.services().query<gui::GuiCompositor>(gui::gui_service_id) != &module.compositor() ||
        manager.services().query<gui::GuiDesktopService>(gui::gui_desktop_service_id) != &module.desktop())
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
        kernel.kernel_modules().services().query<gui::GuiCompositor>(gui::gui_service_id) != &module.compositor() ||
        kernel.kernel_modules().services().query<gui::GuiDesktopService>(gui::gui_desktop_service_id) !=
            &module.desktop() ||
        module.manifest().execution != ModuleExecution::kernel_process ||
        kernel.kernel_modules().kernel_process_pid() == 0 ||
        process == nullptr || process->name() != "mod:kernel-gui" ||
        process->threads().size() != kernel.scheduler().cpu_count() ||
        kernel.kernel_modules().kernel_process_module_count() == 0)
    {
        return Status::fault("kernel GUI module was not started during boot");
    }
    return Status::success();
}

Status test_system_gui_module_loads_after_boot(Kernel &kernel)
{
    if (kernel.kernel_modules().find("system-gui") != nullptr ||
        kernel.kernel_modules().services().contains("gui.system-desktop"))
    {
        return Status::fault("system GUI module was loaded before the OS loader ran");
    }
    if (auto status = stage_os_system_gui_module(kernel); !status.ok())
    {
        return status;
    }

    syscall::Request request{.number = syscall::Number::load_module};
    request.args[0] = reinterpret_cast<u64>(os_system_gui_module_path.data());
    auto response = kernel.syscalls().dispatch(request);
    auto *registered = kernel.kernel_modules().find("system-gui");
    auto module_file = kernel.vfs().read_file(os_system_gui_module_path);
    if (!response.status.ok() || response.value != 0 || !module_file || registered == nullptr ||
        registered->manifest().built_in || registered->state() != ModuleState::started ||
        !kernel.kernel_modules().services().contains("gui.system-desktop"))
    {
        return Status::fault("system GUI module was not loaded by the OS module loader syscall");
    }
    if (!file_contains(module_file.value(), "name=system-gui") ||
        !file_contains(module_file.value(), "require=gui.compositor") ||
        !file_contains(module_file.value(), "require=gui.desktop") ||
        !file_contains(module_file.value(), "export=gui.system-desktop") ||
        !file_contains(module_file.value(), "param=entry:oop") ||
        !file_contains(module_file.value(), "param=class:desktop"))
    {
        return Status::fault("system desktop OKMOD package metadata is incomplete");
    }

    auto *desktop = kernel.loaded_gui_desktop_module();
    auto info = find_surface_by_title(kernel, "ObfuscationOS Login");
    if (desktop == nullptr || desktop->desktop_state() != ExternalGuiDesktopState::greeter ||
        kernel.gui().compositor().shell_mode() != gui::GuiShellMode::system_greeter || !info ||
        info.value().title != "ObfuscationOS Login" || info.value().app != gui::TaskbarApp::none ||
        info.value().chrome != gui::SurfaceChrome::plain ||
        info.value().bounds.width != driver::framebuffer_width ||
        info.value().bounds.height != driver::framebuffer_height ||
        kernel.gui().compositor().last_present_checksum() == 0)
    {
        return Status::fault("system GUI did not stop at the pre-desktop root greeter");
    }

    const auto taskbar_y = static_cast<i32>(driver::framebuffer_height - gui::taskbar_height + 6);
    auto launcher = kernel.gui().compositor().taskbar_launcher_at(gui::task_monitor_launcher_x + 4, taskbar_y);
    if (launcher || launcher.status().code() != StatusCode::not_found)
    {
        return Status::fault("kernel taskbar was visible before system GUI login");
    }
    if (find_surface_by_title(kernel, "Tiny Shell") || find_surface_by_title(kernel, "System Settings") ||
        find_surface_by_title(kernel, "Task Manager") || find_surface_by_title(kernel, "About ObfuscationOS") ||
        find_surface_by_title(kernel, "Notes"))
    {
        return Status::fault("system GUI apps were visible before login");
    }
    return Status::success();
}

Status test_system_gui_mouse_login_selects_user_and_starts_desktop_shell(Kernel &kernel)
{
    auto *desktop = kernel.loaded_gui_desktop_module();
    if (desktop == nullptr || desktop->desktop_state() != ExternalGuiDesktopState::greeter)
    {
        return Status::fault("system GUI greeter was not ready before login");
    }

    constexpr u32 card_width = driver::framebuffer_width > 360 ? 360u : driver::framebuffer_width - 24u;
    constexpr u32 card_height = 178u;
    const auto card_x = static_cast<i32>((driver::framebuffer_width - card_width) / 2);
    const auto card_y = static_cast<i32>((driver::framebuffer_height - card_height) / 2);
    const auto dropdown_x = card_x + 104;
    const auto dropdown_y = card_y + 82;
    const auto user_option_x = dropdown_x;
    const auto user_option_y = card_y + 116;
    const auto login_x = card_x + static_cast<i32>(card_width / 2);
    const auto login_y = card_y + 148;

    if (auto status = kernel.handle_gui_mouse_position(dropdown_x, dropdown_y, true); !status.ok())
    {
        return Status::fault("system GUI dropdown press failed");
    }
    if (auto status = kernel.handle_gui_mouse_position(dropdown_x, dropdown_y, false); !status.ok())
    {
        return Status::fault("system GUI dropdown release failed");
    }
    if (auto status = kernel.handle_gui_mouse_position(user_option_x, user_option_y, true); !status.ok())
    {
        return Status::fault("system GUI dropdown user press failed");
    }
    if (auto status = kernel.handle_gui_mouse_position(user_option_x, user_option_y, false); !status.ok())
    {
        return Status::fault("system GUI dropdown user release failed");
    }
    if (auto status = kernel.handle_gui_mouse_position(login_x, login_y, true); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse_position(login_x, login_y, false); !status.ok())
    {
        return Status::fault("system GUI login release failed");
    }
    desktop = kernel.loaded_gui_desktop_module();
    auto shell = desktop != nullptr ? kernel.gui().compositor().surface_info(desktop->dashboard_surface())
                                    : Result<gui::SurfaceInfo>{Status::not_found("missing desktop")};
    if (desktop == nullptr || desktop->desktop_state() != ExternalGuiDesktopState::desktop ||
        kernel.gui().compositor().shell_mode() != gui::GuiShellMode::system_shell ||
        kernel.posix().user_credentials().euid != user::default_user_uid || !shell ||
        shell.value().title != "ObfuscationOS Desktop" || shell.value().app != gui::TaskbarApp::none ||
        shell.value().chrome != gui::SurfaceChrome::plain ||
        !kernel.kernel_modules().services().contains("gui.app.shell") ||
        !kernel.kernel_modules().services().contains("gui.app.settings") ||
        !kernel.kernel_modules().services().contains("gui.app.tasks") ||
        !kernel.kernel_modules().services().contains("gui.app.about") ||
        !kernel.kernel_modules().services().contains("gui.app.notes"))
    {
        return Status::fault("system GUI login did not start the selected-user desktop shell and app session");
    }
    const auto taskbar_y = static_cast<i32>(driver::framebuffer_height - gui::taskbar_height + 6);
    auto launcher = kernel.gui().compositor().taskbar_launcher_at(gui::task_monitor_launcher_x + 4, taskbar_y);
    if (launcher || launcher.status().code() != StatusCode::not_found)
    {
        return Status::fault("kernel taskbar was still active after system GUI took desktop ownership");
    }
    if (!find_surface_by_title(kernel, "Tiny Shell") ||
        !find_surface_by_title(kernel, "System Settings") ||
        !find_surface_by_title(kernel, "Task Manager") ||
        !find_surface_by_title(kernel, "About ObfuscationOS") || !find_surface_by_title(kernel, "Notes"))
    {
        return Status::fault("system GUI desktop did not show app windows after login");
    }
    return kernel.posix().set_credentials(user::root_credentials());
}

Status test_system_gui_dock_uses_system_app_launchers(Kernel &kernel)
{
    auto *desktop = kernel.loaded_gui_desktop_module();
    if (desktop == nullptr || desktop->desktop_state() != ExternalGuiDesktopState::desktop)
    {
        return Status::fault("system GUI desktop was not ready for dock input");
    }
    auto shell_app = find_surface_by_title(kernel, "Tiny Shell");
    auto task_app = find_surface_by_title(kernel, "Task Manager");
    if (!shell_app || !task_app)
    {
        return Status::fault("system Shell and Task Manager apps were not loaded before dock interaction");
    }

    const auto shell_surface = kernel.debug_shell().gui_surface_id();
    const auto files_surface = kernel.file_manager().surface_id();
    const auto tasks_surface = kernel.task_manager().surface_id();
    const auto system_shell_x = 32;
    const auto dock_y = static_cast<i32>(driver::framebuffer_height - 18);
    if (auto status = kernel.handle_gui_mouse_position(system_shell_x, dock_y, true); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_mouse_position(system_shell_x, dock_y, false); !status.ok())
    {
        return status;
    }

    auto active = kernel.gui().compositor().surface_info(kernel.gui().compositor().active_surface());
    if (!active || active.value().title != "Tiny Shell")
    {
        return Status::fault("system dock did not focus the Tiny Shell app launcher");
    }
    if (kernel.debug_shell().gui_surface_id() != shell_surface || kernel.file_manager().surface_id() != files_surface ||
        kernel.task_manager().surface_id() != tasks_surface)
    {
        return Status::fault("system dock opened a kernel GUI app instead of a system app");
    }
    return Status::success();
}

Status test_system_gui_apps_load_from_rootfs_packages(Kernel &kernel)
{
    auto stat = kernel.simplefs().stat(os_about_app_module_flat_path);
    if (!stat)
    {
        if (stat.status().code() != StatusCode::not_found)
        {
            return stat.status();
        }
        if (auto status = kernel.simplefs().create(os_about_app_module_flat_path, fs::NodeType::regular);
            !status.ok())
        {
            return status;
        }
    }
    if (auto status = kernel.simplefs().write_file(os_about_app_module_flat_path, bytes_for(os_about_app_module_text));
        !status.ok())
    {
        return status;
    }

    syscall::Request request{.number = syscall::Number::load_module};
    request.args[0] = reinterpret_cast<u64>(os_about_app_module_path.data());
    auto response = kernel.syscalls().dispatch(request);
    auto *registered = kernel.kernel_modules().find("system-about");
    if (!response.status.ok() || response.value != 0 || registered == nullptr ||
        registered->manifest().built_in || registered->state() != ModuleState::started ||
        !kernel.kernel_modules().services().contains("gui.app.about"))
    {
        return Status::fault("system GUI app module was not loaded from the rootfs package");
    }

    auto info = find_surface_by_title(kernel, "About ObfuscationOS");
    if (!info || info.value().title != "About ObfuscationOS" ||
        kernel.gui().compositor().last_present_checksum() == 0)
    {
        return Status::fault("system GUI app module did not open its app surface");
    }
    return Status::success();
}

Status test_system_gui_apps_load_from_distro_fallback_packages(Kernel &kernel)
{
    syscall::Request request{.number = syscall::Number::load_module};
    request.args[0] = reinterpret_cast<u64>("/boot/modules/apps/settings.okmod");
    auto response = kernel.syscalls().dispatch(request);
    auto *registered = kernel.kernel_modules().find("system-settings");
    if (!response.status.ok() || response.value != 0 || registered == nullptr ||
        registered->manifest().built_in || registered->state() != ModuleState::started ||
        !kernel.kernel_modules().services().contains("gui.app.settings"))
    {
        return Status::fault("system GUI app fallback package was not loaded");
    }

    auto info = find_surface_by_title(kernel, "System Settings");
    if (!info || info.value().title != "System Settings" ||
        kernel.gui().compositor().last_present_checksum() == 0)
    {
        return Status::fault("system GUI app fallback did not open its app surface");
    }
    return Status::success();
}

Status test_close_debug_gui_restores_system_desktop(Kernel &kernel)
{
    auto *desktop = kernel.loaded_gui_desktop_module();
    if (desktop == nullptr || desktop->dashboard_surface() == 0)
    {
        return Status::fault("system desktop module was not available for debug cleanup");
    }
    if (auto status = kernel.gui().compositor().destroy_surface(desktop->dashboard_surface()); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.debug_shell().show_gui(); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.close_debug_gui(); !status.ok())
    {
        return status;
    }

    auto info = find_surface_by_title(kernel, "ObfuscationOS Desktop");
    if (!info || kernel.debug_shell().gui_open())
    {
        return Status::fault("debug GUI cleanup did not restore the system desktop");
    }
    return Status::success();
}

Status test_kernel_file_manager_draws_vfs(Kernel &kernel)
{
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
    const auto *process = kernel.scheduler().find(kernel.file_manager().process_id());
    auto info = compositor.surface_info(kernel.file_manager().surface_id());
    if (kernel.file_manager().surface_id() == 0 || kernel.file_manager().path() != "/" ||
        kernel.file_manager().render_count() == 0 || !info ||
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
    if (!process->credentials().kernel_space || process->execution() != sched::ProcessExecution::kernel_thread ||
        process->address_space_id() != 0)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager process did not keep kernel-thread credentials");
    }
    const auto first_manager_pid = kernel.file_manager().process_id();
    const auto first_manager_surface = kernel.file_manager().surface_id();
    if (auto status = kernel.posix().set_credentials(user::root_credentials()); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.open_file_manager("/tmp"); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    const auto second_manager_pid = kernel.file_manager().process_id();
    const auto second_manager_surface = kernel.file_manager().surface_id();
    const auto *second_process = kernel.scheduler().find(second_manager_pid);
    if (first_manager_pid == second_manager_pid || first_manager_surface == second_manager_surface ||
        kernel.scheduler().find(first_manager_pid) == nullptr ||
        second_process == nullptr || !compositor.surface_info(first_manager_surface) ||
        !compositor.surface_info(second_manager_surface))
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager did not keep multiple windows alive");
    }
    if (second_process->name() != "fm:root" || second_process->credentials().kernel_space ||
        second_process->execution() != sched::ProcessExecution::user_process ||
        second_process->address_space_id() == 0)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("root-launched file manager was not an isolated user process");
    }
    if (auto status = kernel.close_file_manager(); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.posix().set_credentials(user::kernel_credentials()); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.file_manager().process_id() != first_manager_pid || kernel.file_manager().path() != "/" ||
        kernel.scheduler().find(first_manager_pid) == nullptr || compositor.surface_info(second_manager_surface))
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("closing active file manager did not preserve the earlier window");
    }
    const auto resize_render_count = kernel.file_manager().render_count();
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
    info = compositor.surface_info(kernel.file_manager().surface_id());
    const auto expected_maximized_height = compositor.shell_mode() == gui::GuiShellMode::kernel_shell
                                               ? driver::framebuffer_height - gui::taskbar_height
                                               : driver::framebuffer_height;
    if (!info || info.value().window_state != gui::WindowState::maximized ||
        info.value().bounds.width != driver::framebuffer_width ||
        info.value().bounds.height != expected_maximized_height ||
        kernel.file_manager().render_count() <= resize_render_count)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager maximize did not resize and redraw");
    }
    const auto before_render_count = kernel.file_manager().render_count();
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
    if (kernel.file_manager().path() != "/tmp" || kernel.file_manager().render_count() <= before_render_count)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager mouse navigation did not open /tmp");
    }
    const auto before_parent_render_count = kernel.file_manager().render_count();
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
    if (kernel.file_manager().path() != "/" ||
        kernel.file_manager().render_count() <= before_parent_render_count)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("GUI file manager parent navigation did not return to /");
    }
    const auto manager_pid = kernel.file_manager().process_id();
    const auto manager_surface = kernel.file_manager().surface_id();
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
    auto shell_kernel = kernel.debug_shell().execute("su kernel");
    if (!shell_kernel || !contains_text(shell_kernel.value(), "kernel"))
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("debug shell did not enter kernel session before killing GUI file manager process");
    }
    auto killed = kernel.debug_shell().execute(kill_command.view());
    static_cast<void>(kernel.debug_shell().execute("exit"));
    static_cast<void>(kernel.posix().set_credentials(user::kernel_credentials()));
    const auto *killed_process = kernel.scheduler().find(manager_pid);
    if (!killed)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return killed.status();
    }
    if (!killed.value().empty())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("debug shell kill returned unexpected output for GUI file manager process");
    }
    if (killed_process != nullptr)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("debug shell kill did not remove the GUI file manager scheduler process");
    }
    if (kernel.file_manager().surface_id() != 0 || kernel.file_manager().process_id() != 0)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("debug shell kill did not clear the active GUI file manager state");
    }
    if (compositor.surface_info(manager_surface))
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("debug shell kill did not destroy the GUI file manager surface");
    }
    if (auto status = kernel.open_file_manager("/"); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    info = compositor.surface_info(kernel.file_manager().surface_id());
    const auto close_pid = kernel.file_manager().process_id();
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
    if (kernel.file_manager().surface_id() != 0 || kernel.file_manager().process_id() != 0 ||
        kernel.scheduler().find(close_pid) != nullptr)
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

Status test_kernel_task_manager_draws_usage(Kernel &kernel)
{
    auto &compositor = kernel.gui().compositor();
    const auto saved_credentials = kernel.posix().user_credentials();
    if (auto status = kernel.posix().set_credentials(user::kernel_credentials()); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.debug_shell().close_all_gui(); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.debug_shell().show_gui(); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }

    auto tui = kernel.debug_shell().execute("taskman");
    if (!tui || !contains_text(tui.value(), "TASK MANAGER") || !contains_text(tui.value(), "CPU") ||
        !contains_text(tui.value(), "NET") || !contains_text(tui.value(), "DISK"))
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("task manager TUI did not render CPU, network, and disk usage");
    }

    if (auto status = kernel.open_task_manager(); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    const auto pid = kernel.task_manager().process_id();
    const auto surface = kernel.task_manager().surface_id();
    const auto *process = kernel.scheduler().find(pid);
    auto info = compositor.surface_info(surface);
    if (surface == 0 || pid == 0 || process == nullptr || process->name() != "tm:kernel" ||
        kernel.task_manager().render_count() == 0 || !info || compositor.last_present_checksum() == 0)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("task manager GUI did not create a surface and process");
    }

    const auto before_render_count = kernel.task_manager().render_count();
    if (auto status = compositor.resize_surface(
            surface, gui::Rect{.x = info.value().bounds.x, .y = info.value().bounds.y, .width = 340, .height = 188});
        !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (auto status = kernel.handle_gui_mouse(0, 0, false); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.task_manager().render_count() <= before_render_count)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("task manager GUI did not redraw after resize");
    }

    if (auto status = kernel.close_task_manager(); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.task_manager().surface_id() != 0 || kernel.task_manager().process_id() != 0 ||
        kernel.scheduler().find(pid) != nullptr || compositor.surface_info(surface))
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("task manager GUI did not close its process and surface");
    }

    const auto top_render_before = kernel.task_manager().render_count();
    auto top = kernel.debug_shell().execute("top");
    const auto top_render_after_start = kernel.task_manager().render_count();
    const auto top_pid = kernel.debug_shell().foreground_process_id();
    const auto *top_process = kernel.scheduler().find(top_pid);
    const auto *launching_shell_process = kernel.scheduler().find(kernel.debug_shell().process_id());
    const auto top_surface = kernel.task_manager().surface_id();
    auto top_info = compositor.surface_info(top_surface);
    if (!top || top_pid == 0 || top_pid != kernel.task_manager().process_id() || top_process == nullptr ||
        top_process->name() != "top:kernel" || top_surface == 0 || !top_info || top_info.value().title != "top" ||
        top_info.value().app != gui::TaskbarApp::task_monitor || top_render_after_start <= top_render_before)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top did not enter a foreground task-manager program");
    }
    if (launching_shell_process == nullptr || launching_shell_process->state() == sched::ProcessState::blocked)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("foreground top incorrectly blocked its launching shell process");
    }
    auto blocked = kernel.debug_shell().execute("echo blocked");
    if (blocked || blocked.status().code() != StatusCode::would_block)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top did not keep shell commands waiting while realtime view was active");
    }
    if (auto status = kernel.tick(); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.task_manager().render_count() <= top_render_after_start)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top did not refresh its realtime display");
    }
    if (auto status = kernel.task_manager().refresh(compositor, kernel); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.task_manager().sampled_cpu_usage_percent(0) != 0 ||
        kernel.task_manager().sampled_process_usage_percent(top_pid) != 0)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top CPU display used cumulative counters instead of refresh deltas");
    }
    const auto after_tick_render_count = kernel.task_manager().render_count();
    const auto before_mouse_wheel_scroll = kernel.task_manager().process_scroll();
    if (auto status = kernel.handle_gui_scroll(gui::scroll_rows(gui::ScrollDirection::next)); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.task_manager().render_count() <= after_tick_render_count)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top did not redraw when its process list was scrolled");
    }
    if (kernel.task_manager().process_scroll() <= before_mouse_wheel_scroll)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top mouse wheel down did not follow the GUI scroll convention");
    }
    const auto after_mouse_wheel_down = kernel.task_manager().process_scroll();
    if (auto status = kernel.handle_gui_scroll(gui::scroll_rows(gui::ScrollDirection::previous)); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.task_manager().process_scroll() >= after_mouse_wheel_down)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top mouse wheel up did not follow the GUI scroll convention");
    }
    if (auto status = kernel.handle_gui_key(0x03); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.debug_shell().foreground_process_id() != 0 || kernel.task_manager().surface_id() != 0 ||
        kernel.scheduler().find(top_pid) != nullptr)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top did not exit on interrupt");
    }
    auto resumed = kernel.debug_shell().execute("echo top-resumed");
    if (!resumed || resumed.value() != "top-resumed\n")
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("shell did not resume after top was interrupted");
    }
    auto top_again = kernel.debug_shell().execute("top");
    const auto closing_shell_pid = kernel.debug_shell().process_id();
    const auto closing_shell_surface = kernel.debug_shell().gui_surface_id();
    const auto closing_top_pid = kernel.debug_shell().foreground_process_id();
    if (!top_again || closing_shell_pid == 0 || closing_shell_surface == 0 || closing_top_pid == 0 ||
        closing_top_pid != kernel.task_manager().process_id())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top relaunch did not attach to the GUI shell foreground");
    }
    if (auto status = kernel.handle_gui_key(ok_input_open_shell); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    auto second_top = kernel.debug_shell().execute("top");
    const auto second_shell_pid = kernel.debug_shell().process_id();
    const auto second_shell_surface = kernel.debug_shell().gui_surface_id();
    const auto second_top_pid = kernel.debug_shell().foreground_process_id();
    if (!second_top || second_shell_pid == 0 || second_shell_surface == 0 || second_shell_pid == closing_shell_pid ||
        second_top_pid == 0 || second_top_pid == closing_top_pid || process_count_named(kernel.scheduler(), "top:kernel") != 2)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("top did not create an independent monitor for a second shell");
    }
    if (auto status = kernel.debug_shell().close_surface_window(second_shell_surface); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.scheduler().find(second_shell_pid) != nullptr || kernel.scheduler().find(second_top_pid) != nullptr ||
        kernel.scheduler().find(closing_shell_pid) == nullptr || kernel.scheduler().find(closing_top_pid) == nullptr ||
        process_count_named(kernel.scheduler(), "top:kernel") != 1)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("closing one shell top instance affected another shell's top");
    }
    if (auto status = kernel.debug_shell().close_surface_window(closing_shell_surface); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    if (kernel.scheduler().find(closing_shell_pid) != nullptr || kernel.scheduler().find(closing_top_pid) != nullptr ||
        kernel.task_manager().surface_id() != 0 || process_count_named(kernel.scheduler(), "oksh") != 0 ||
        process_count_named(kernel.scheduler(), "top:kernel") != 0)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("closing a shell with foreground top left a child or orphan oksh behind");
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
    const auto expected_shell_height = kernel.gui().compositor().shell_mode() == gui::GuiShellMode::kernel_shell
                                           ? driver::framebuffer_height - gui::taskbar_height
                                           : driver::framebuffer_height;
    if (!surface || surface.value().window_state != gui::WindowState::maximized ||
        surface.value().bounds.width != driver::framebuffer_width ||
        surface.value().bounds.height != expected_shell_height)
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
    auto first_user = kernel.debug_shell().execute("su user");
    if (!first_user || first_user.value() != "user\n")
    {
        return Status::fault("debug shell first GUI session did not switch user");
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
    auto second_user = kernel.debug_shell().execute("whoami");
    const auto second_shell_surface = kernel.debug_shell().gui_surface_id();
    const auto second_shell_pid = kernel.debug_shell().process_id();
    const auto *first_process = kernel.scheduler().find(first_shell_pid);
    const auto *second_process = kernel.scheduler().find(second_shell_pid);
    if (!second_user || second_user.value() != "user\n" || first_process == nullptr ||
        second_process == nullptr || first_process->credentials().euid != user::default_user_uid ||
        second_process->credentials().euid != user::default_user_uid ||
        second_process->execution() != sched::ProcessExecution::user_process ||
        second_process->address_space_id() == 0)
    {
        return Status::fault("user-launched GUI shell was not an isolated user process");
    }
    if (auto status = kernel.gui().compositor().raise_surface(first_shell_surface); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_key('\r'); !status.ok())
    {
        return status;
    }
    auto restored_first_user = kernel.debug_shell().execute("exit");
    if (!restored_first_user || restored_first_user.value() != "root\n")
    {
        return Status::fault("GUI shell did not restore and exit its per-window user session");
    }
    if (auto status = kernel.gui().compositor().raise_surface(second_shell_surface); !status.ok())
    {
        return status;
    }
    if (auto status = kernel.handle_gui_key('\r'); !status.ok())
    {
        return status;
    }
    second_user = kernel.debug_shell().execute("whoami");
    if (!second_user || second_user.value() != "user\n")
    {
        return Status::fault("GUI shell user switch leaked across windows");
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
    auto reset_user = kernel.debug_shell().execute("exit");
    if (!reset_user || !reset_user.value().empty() || kernel.debug_shell().gui_open())
    {
        return Status::fault("debug shell GUI session did not close after user process test");
    }
    auto root_user = kernel.debug_shell().execute("whoami");
    if (!root_user || root_user.value() != "root\n")
    {
        return Status::fault("debug shell GUI session did not reset to the root default");
    }
    return Status::success();
}

Status test_gui_desktop_service_boundary(Kernel &kernel)
{
    auto *desktop =
        kernel.kernel_modules().services().query<gui::GuiDesktopService>(gui::gui_desktop_service_id);
    auto *compositor = kernel.kernel_modules().services().query<gui::GuiCompositor>(gui::gui_service_id);
    if (desktop == nullptr || compositor == nullptr || desktop != &kernel.gui().desktop() ||
        compositor != &kernel.gui().compositor() || !desktop->bound() ||
        desktop->backend() != gui::DesktopBackend::kernel_compositor)
    {
        return Status::fault("GUI desktop service was not published through the module service boundary");
    }

    const auto before = desktop->window_count();
    auto window = desktop->open_window(gui::DesktopWindowRequest{
        .bounds = gui::Rect{.x = 22, .y = 18, .width = 80, .height = 48},
        .title = "desktop-api",
        .app = gui::TaskbarApp::task_monitor,
    });
    if (!window)
    {
        return window.status();
    }
    if (desktop->window_count() != before + 1 || desktop->active_window() != window.value())
    {
        return Status::fault("GUI desktop service did not open or focus a window");
    }
    if (auto status = desktop->route_key('x'); !status.ok())
    {
        return status;
    }
    if (desktop->routed_key_count() != 1)
    {
        return Status::fault("GUI desktop service did not track routed key events");
    }
    const auto scanout = desktop->scanout();
    if (!scanout.active || scanout.width != driver::framebuffer_width || scanout.height != driver::framebuffer_height)
    {
        return Status::fault("GUI hybrid scanout did not expose the kernel compositor framebuffer");
    }
    if (auto status = desktop->configure_scanout(gui::GuiScanout{
            .width = 960,
            .height = 540,
            .pitch = 960 * 4,
            .bytes_per_pixel = 4,
        });
        !status.ok())
    {
        return status;
    }
    if (!desktop->scanout().active || desktop->scanout().width != 960)
    {
        return Status::fault("GUI hybrid scanout reconfiguration failed");
    }
    auto shared = desktop->allocate_shared_buffer(128);
    if (!shared)
    {
        return shared.status();
    }
    auto shared_buffer = desktop->shared_buffer(shared.value());
    if (!shared_buffer || !shared_buffer.value()->mapped || shared_buffer.value()->size != 128)
    {
        return Status::fault("GUI shared-buffer allocation failed");
    }
    shared_buffer.value()->bytes[0] = std::byte{0x5a};
    if (desktop->shared_buffer_count() != 1 || desktop->shared_buffer(shared.value()).value()->bytes[0] != std::byte{0x5a})
    {
        return Status::fault("GUI shared-buffer lookup failed");
    }
    if (auto status = desktop->write_clipboard("ok-clipboard"); !status.ok())
    {
        return status;
    }
    if (desktop->clipboard_text() != "ok-clipboard")
    {
        return Status::fault("GUI clipboard ABI validation failed");
    }
    if (auto status = desktop->route_input(gui::GuiInputEvent{.kind = gui::GuiInputEventKind::pointer_position,
                                                              .x = 33,
                                                              .y = 44});
        !status.ok())
    {
        return status;
    }
    if (desktop->cursor().x != 33 || desktop->cursor().y != 44 || desktop->routed_input_count() < 2)
    {
        return Status::fault("GUI input ABI route validation failed");
    }
    if (auto status = desktop->release_shared_buffer(shared.value()); !status.ok())
    {
        return status;
    }
    if (desktop->shared_buffer_count() != 0)
    {
        return Status::fault("GUI shared-buffer release failed");
    }
    if (auto status = desktop->focus_window(window.value()); !status.ok())
    {
        return status;
    }
    if (auto status = desktop->close_window(window.value()); !status.ok())
    {
        return status;
    }
    return desktop->window_count() == before ? Status::success()
                                             : Status::fault("GUI desktop service did not close its window");
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
    if (auto status = test_kernel_gui_mouse_position_uses_absolute_pointer(kernel); !status.ok())
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
    if (auto status = test_system_gui_module_loads_after_boot(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_system_gui_mouse_login_selects_user_and_starts_desktop_shell(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_system_gui_dock_uses_system_app_launchers(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_system_gui_apps_load_from_rootfs_packages(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_system_gui_apps_load_from_distro_fallback_packages(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_close_debug_gui_restores_system_desktop(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_kernel_file_manager_draws_vfs(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_kernel_task_manager_draws_usage(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_shell_renders_to_gui(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_gui_desktop_service_boundary(kernel); !status.ok())
    {
        return status;
    }

    report.gui = true;
    return Status::success();
}

} // namespace ok
