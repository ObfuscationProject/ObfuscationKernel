#include "roadmap_tests.hpp"

#include "ok/gui/gui.hpp"

namespace ok
{
namespace
{

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

Status test_kernel_gui_is_started(Kernel &kernel)
{
    auto &module = kernel.gui();
    if (module.state() != ModuleState::started || module.compositor().state() != gui::GuiState::running ||
        module.compositor().last_present_checksum() == 0 ||
        kernel.kernel_modules().services().query<gui::GuiModule>(gui::gui_service_id) != &module)
    {
        return Status::fault("kernel GUI module was not started during boot");
    }
    return Status::success();
}

Status test_shell_renders_to_gui(Kernel &kernel)
{
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
    if (!surface || surface.value().bounds.x != 0 || surface.value().bounds.y != 0 ||
        surface.value().bounds.width != driver::framebuffer_width ||
        surface.value().bounds.height != driver::framebuffer_height)
    {
        return Status::fault("debug shell GUI surface is not maximized");
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
    if (auto status = test_gui_module_restarts_after_crash(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_kernel_gui_is_started(kernel); !status.ok())
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
