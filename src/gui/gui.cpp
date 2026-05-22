#include "ok/gui/gui.hpp"

namespace ok::gui
{
namespace
{

constexpr u32 default_surface_color = 0xff18343fu;
constexpr u32 frame_color = 0xffd8f3ffu;
constexpr u32 title_color = 0xff44aa88u;

[[nodiscard]] i32 min_i32(i32 left, i32 right)
{
    return left < right ? left : right;
}

[[nodiscard]] i32 max_i32(i32 left, i32 right)
{
    return left > right ? left : right;
}

} // namespace

Status GuiCompositor::start(driver::FramebufferDisplayDriver &display)
{
    if (state_ == GuiState::running)
    {
        return Status::busy("GUI compositor is already running");
    }
    display_ = &display;
    state_ = GuiState::running;
    crash_reason_.clear();
    reset_surfaces();
    ++generation_;
    last_present_checksum_ = display_->checksum();
    return Status::success();
}

Status GuiCompositor::stop()
{
    reset_surfaces();
    state_ = GuiState::stopped;
    last_present_checksum_ = 0;
    return Status::success();
}

Status GuiCompositor::simulate_crash(std::string_view reason)
{
    if (state_ != GuiState::running)
    {
        return Status::not_initialized("GUI compositor is not running");
    }
    state_ = GuiState::crashed;
    crash_reason_.clear();
    static_cast<void>(crash_reason_.assign(reason));
    return Status::success();
}

Status GuiCompositor::ensure_running() const
{
    if (state_ == GuiState::crashed)
    {
        return Status::fault("GUI compositor crashed");
    }
    if (state_ != GuiState::running || display_ == nullptr)
    {
        return Status::not_initialized("GUI compositor is not running");
    }
    return Status::success();
}

Status GuiCompositor::validate_bounds(Rect bounds) const
{
    if (bounds.width == 0 || bounds.height == 0)
    {
        return Status::invalid_argument("GUI surface has empty bounds");
    }
    if (bounds.width > max_gui_surface_width || bounds.height > max_gui_surface_height)
    {
        return Status::invalid_argument("GUI surface exceeds fixed backing store");
    }
    return Status::success();
}

void GuiCompositor::reset_surfaces()
{
    surfaces_ = {};
    next_surface_id_ = 1;
    next_z_index_ = 1;
}

Result<usize> GuiCompositor::find_surface_index(SurfaceId id) const
{
    for (usize i = 0; i < surfaces_.size(); ++i)
    {
        if (surfaces_[i].id == id)
        {
            return i;
        }
    }
    return Status::not_found("GUI surface not found");
}

Result<SurfaceId> GuiCompositor::create_surface(Rect bounds, std::string_view title)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    if (auto status = validate_bounds(bounds); !status.ok())
    {
        return status;
    }
    if (surfaces_.full())
    {
        return Status::overflow("GUI surface table is full");
    }

    Surface surface{};
    surface.id = next_surface_id_;
    surface.bounds = bounds;
    surface.z_index = next_z_index_;
    surface.visible = true;
    if (auto status = surface.title.assign(title); !status.ok())
    {
        return status;
    }
    for (auto &pixel : surface.pixels)
    {
        pixel = default_surface_color;
    }

    const auto id = surface.id;
    if (auto status = surfaces_.push_back(surface); !status.ok())
    {
        return status;
    }
    ++next_surface_id_;
    ++next_z_index_;
    return id;
}

Status GuiCompositor::destroy_surface(SurfaceId id)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    auto index = find_surface_index(id);
    if (!index)
    {
        return index.status();
    }
    return surfaces_.erase_at(index.value());
}

Status GuiCompositor::set_visible(SurfaceId id, bool visible)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    auto index = find_surface_index(id);
    if (!index)
    {
        return index.status();
    }
    surfaces_[index.value()].visible = visible;
    return Status::success();
}

Status GuiCompositor::move_surface(SurfaceId id, i32 x, i32 y)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    auto index = find_surface_index(id);
    if (!index)
    {
        return index.status();
    }
    auto &surface = surfaces_[index.value()];
    surface.bounds.x = x;
    surface.bounds.y = y;
    return Status::success();
}

Status GuiCompositor::fill(SurfaceId id, u32 rgba)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    auto index = find_surface_index(id);
    if (!index)
    {
        return index.status();
    }
    for (auto &pixel : surfaces_[index.value()].pixels)
    {
        pixel = rgba;
    }
    return Status::success();
}

Status GuiCompositor::fill_rect(SurfaceId id, Rect rect, u32 rgba)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    if (rect.width == 0 || rect.height == 0)
    {
        return Status::invalid_argument("GUI rectangle has empty bounds");
    }
    auto index = find_surface_index(id);
    if (!index)
    {
        return index.status();
    }
    auto &surface = surfaces_[index.value()];
    const auto right = rect.x + static_cast<i32>(rect.width);
    const auto bottom = rect.y + static_cast<i32>(rect.height);
    const auto clipped_left = max_i32(rect.x, 0);
    const auto clipped_top = max_i32(rect.y, 0);
    const auto clipped_right = min_i32(right, static_cast<i32>(surface.bounds.width));
    const auto clipped_bottom = min_i32(bottom, static_cast<i32>(surface.bounds.height));
    if (clipped_left >= clipped_right || clipped_top >= clipped_bottom)
    {
        return Status::success();
    }

    for (i32 y = clipped_top; y < clipped_bottom; ++y)
    {
        for (i32 x = clipped_left; x < clipped_right; ++x)
        {
            surface.pixels[static_cast<usize>(y) * max_gui_surface_width + static_cast<usize>(x)] = rgba;
        }
    }
    return Status::success();
}

Status GuiCompositor::put_pixel(SurfaceId id, u32 x, u32 y, u32 rgba)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    auto index = find_surface_index(id);
    if (!index)
    {
        return index.status();
    }
    auto &surface = surfaces_[index.value()];
    if (x >= surface.bounds.width || y >= surface.bounds.height)
    {
        return Status::invalid_argument("GUI pixel coordinate out of range");
    }
    surface.pixels[static_cast<usize>(y) * max_gui_surface_width + x] = rgba;
    return Status::success();
}

Status GuiCompositor::present()
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }

    const auto mode = display_->mode();
    for (const auto &surface : surfaces_)
    {
        if (!surface.visible)
        {
            continue;
        }
        for (u32 y = 0; y < surface.bounds.height; ++y)
        {
            const auto target_y = surface.bounds.y + static_cast<i32>(y);
            if (target_y < 0 || target_y >= static_cast<i32>(mode.height))
            {
                continue;
            }
            for (u32 x = 0; x < surface.bounds.width; ++x)
            {
                const auto target_x = surface.bounds.x + static_cast<i32>(x);
                if (target_x < 0 || target_x >= static_cast<i32>(mode.width))
                {
                    continue;
                }
                auto color = surface.pixels[static_cast<usize>(y) * max_gui_surface_width + x];
                if (x == 0 || y == 0 || x + 1 == surface.bounds.width || y + 1 == surface.bounds.height)
                {
                    color = frame_color;
                }
                else if (y == 1)
                {
                    color = title_color;
                }
                if (auto status =
                        display_->put_pixel(static_cast<u32>(target_x), static_cast<u32>(target_y), color);
                    !status.ok())
                {
                    return status;
                }
            }
        }
    }

    last_present_checksum_ = display_->checksum();
    return Status::success();
}

Result<SurfaceInfo> GuiCompositor::surface_info(SurfaceId id) const
{
    auto index = find_surface_index(id);
    if (!index)
    {
        return index.status();
    }
    const auto &surface = surfaces_[index.value()];
    return SurfaceInfo{
        .id = surface.id,
        .bounds = surface.bounds,
        .z_index = surface.z_index,
        .visible = surface.visible,
        .title = surface.title.view(),
    };
}

ModuleManifest GuiModule::manifest() const
{
    static constexpr std::array exports{gui_service_id};
    return ModuleManifest{
        .name = gui_module_name,
        .version = "1",
        .module_class = "gui",
        .dependencies = {},
        .exported_services = exports,
        .required_services = {},
        .built_in = true,
        .init_priority = 75,
    };
}

Status GuiModule::start(ServiceRegistry &)
{
    if (display_ == nullptr)
    {
        return Status::not_initialized("GUI module has no display binding");
    }
    return compositor_.start(*display_);
}

Status GuiModule::stop()
{
    return compositor_.stop();
}

Status GuiModule::shutdown()
{
    return stop();
}

Status GuiSupervisor::tick()
{
    if (!module_.compositor().crashed())
    {
        return Status::success();
    }
    ++restart_attempts_;
    return modules_.restart_module(gui_module_name);
}

std::string_view gui_state_name(GuiState state)
{
    switch (state)
    {
    case GuiState::stopped:
        return "stopped";
    case GuiState::running:
        return "running";
    case GuiState::crashed:
        return "crashed";
    }
    return "unknown";
}

} // namespace ok::gui
