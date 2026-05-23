#include "ok/gui/gui.hpp"

extern "C" void ok_platform_display_gui_pixel(ok::u32 logical_width, ok::u32 logical_height, ok::u32 x, ok::u32 y,
                                              ok::u32 color) __attribute__((weak));

namespace ok::gui
{
namespace
{

constexpr u32 desktop_background_color = 0xff061018u;
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
    surfaces_.clear();
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

    auto surface = surfaces_.push_back_slot();
    if (!surface)
    {
        return surface.status();
    }

    auto &slot = *surface.value();
    slot.id = next_surface_id_;
    slot.bounds = bounds;
    slot.z_index = next_z_index_;
    slot.visible = true;
    if (auto status = slot.title.assign(title); !status.ok())
    {
        static_cast<void>(surfaces_.erase_at(surfaces_.size() - 1));
        return status;
    }
    for (auto &pixel : slot.pixels)
    {
        pixel = default_surface_color;
    }

    const auto id = slot.id;
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

Status GuiCompositor::set_title(SurfaceId id, std::string_view title)
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
    return surfaces_[index.value()].title.assign(title);
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

Status GuiCompositor::resize_surface(SurfaceId id, Rect bounds)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    if (auto status = validate_bounds(bounds); !status.ok())
    {
        return status;
    }
    auto index = find_surface_index(id);
    if (!index)
    {
        return index.status();
    }
    auto &surface = surfaces_[index.value()];
    const auto old_bounds = surface.bounds;
    for (u32 y = 0; y < bounds.height; ++y)
    {
        for (u32 x = 0; x < bounds.width; ++x)
        {
            if (x >= old_bounds.width || y >= old_bounds.height)
            {
                surface.pixels[static_cast<usize>(y) * max_gui_surface_width + x] = default_surface_color;
            }
        }
    }
    surface.bounds = bounds;
    return Status::success();
}

Status GuiCompositor::raise_surface(SurfaceId id)
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
    surfaces_[index.value()].z_index = next_z_index_;
    ++next_z_index_;
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

void GuiCompositor::draw_cell(Surface &surface, u32 column, u32 row, char value, u32 foreground, u32 background)
{
    const auto origin_x = column * gui_glyph_width;
    const auto origin_y = row * gui_glyph_height;
    for (u32 y = 0; y < gui_glyph_height; ++y)
    {
        const auto target_y = origin_y + y;
        if (target_y >= surface.bounds.height)
        {
            continue;
        }
        const auto row_bits = driver::BitmapFontRenderer::glyph_row(value, y);
        for (u32 x = 0; x < gui_glyph_width; ++x)
        {
            const auto target_x = origin_x + x;
            if (target_x >= surface.bounds.width)
            {
                continue;
            }
            const auto bit = x < driver::BitmapFontRenderer::glyph_width &&
                             y < driver::BitmapFontRenderer::glyph_height &&
                             ((row_bits >> (driver::BitmapFontRenderer::glyph_width - 1 - x)) & 1u) != 0;
            surface.pixels[static_cast<usize>(target_y) * max_gui_surface_width + target_x] =
                bit ? foreground : background;
        }
    }
}

Status GuiCompositor::draw_text(SurfaceId id, u32 column, u32 row, std::string_view text, u32 foreground,
                                u32 background)
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
    const auto first_column = column;
    const auto columns = surface.bounds.width / gui_glyph_width;
    const auto rows = surface.bounds.height / gui_glyph_height;
    if (column >= columns || row >= rows)
    {
        return Status::success();
    }
    for (const auto value : text)
    {
        if (value == '\f')
        {
            for (auto &pixel : surface.pixels)
            {
                pixel = background;
            }
            column = first_column;
            row = 0;
            continue;
        }
        if (value == '\n')
        {
            column = first_column;
            ++row;
            if (row >= rows)
            {
                break;
            }
            continue;
        }
        if (column >= columns)
        {
            column = first_column;
            ++row;
            if (row >= rows)
            {
                break;
            }
        }
        draw_cell(surface, column, row, value, foreground, background);
        ++column;
    }
    return Status::success();
}

Status GuiCompositor::draw_surface(const Surface &surface)
{
    if (!surface.visible)
    {
        return Status::success();
    }
    const auto mode = display_->mode();
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
            if (auto status = display_->put_pixel(static_cast<u32>(target_x), static_cast<u32>(target_y), color);
                !status.ok())
            {
                return status;
            }
            if (ok_platform_display_gui_pixel != nullptr)
            {
                ok_platform_display_gui_pixel(mode.width, mode.height, static_cast<u32>(target_x),
                                              static_cast<u32>(target_y), color);
            }
        }
    }
    return Status::success();
}

Status GuiCompositor::present()
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }

    const auto mode = display_->mode();
    for (u32 y = 0; y < mode.height; ++y)
    {
        for (u32 x = 0; x < mode.width; ++x)
        {
            if (auto status = display_->put_pixel(x, y, desktop_background_color); !status.ok())
            {
                return status;
            }
            if (ok_platform_display_gui_pixel != nullptr)
            {
                ok_platform_display_gui_pixel(mode.width, mode.height, x, y, desktop_background_color);
            }
        }
    }

    u32 last_z_index = 0;
    for (usize drawn = 0; drawn < surfaces_.size(); ++drawn)
    {
        const Surface *next = nullptr;
        for (const auto &surface : surfaces_)
        {
            if (!surface.visible || surface.z_index <= last_z_index)
            {
                continue;
            }
            if (next == nullptr || surface.z_index < next->z_index)
            {
                next = &surface;
            }
        }
        if (next == nullptr)
        {
            break;
        }
        if (auto status = draw_surface(*next); !status.ok())
        {
            return status;
        }
        last_z_index = next->z_index;
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

Result<SurfaceId> GuiCompositor::surface_at(i32 x, i32 y) const
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    const Surface *top = nullptr;
    for (const auto &surface : surfaces_)
    {
        if (!surface.visible)
        {
            continue;
        }
        const auto right = surface.bounds.x + static_cast<i32>(surface.bounds.width);
        const auto bottom = surface.bounds.y + static_cast<i32>(surface.bounds.height);
        if (x < surface.bounds.x || y < surface.bounds.y || x >= right || y >= bottom)
        {
            continue;
        }
        if (top == nullptr || surface.z_index > top->z_index)
        {
            top = &surface;
        }
    }
    if (top == nullptr)
    {
        return Status::not_found("no GUI surface at point");
    }
    return top->id;
}

Result<Rect> GuiCompositor::desktop_bounds() const
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    const auto mode = display_->mode();
    return Rect{.x = 0, .y = 0, .width = mode.width, .height = mode.height};
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
        .execution = ModuleExecution::kernel_process,
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
