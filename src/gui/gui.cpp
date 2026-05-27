#include "ok/gui/module.hpp"

namespace ok::gui
{
namespace
{

constexpr u32 desktop_background_color = 0xff061018u;
constexpr u32 default_surface_color = 0xff18343fu;
constexpr u32 frame_color = 0xffd8f3ffu;
constexpr u32 title_frame_color = 0xff44aa88u;
constexpr u32 inactive_title_frame_color = 0xff4f6974u;
constexpr u32 title_color = 0xff12313du;
constexpr u32 inactive_title_color = 0xff1a2630u;
constexpr u32 taskbar_color = 0xff101820u;
constexpr u32 taskbar_edge_color = 0xff44aa88u;
constexpr u32 taskbar_button_color = 0xff1a3140u;
constexpr u32 active_taskbar_button_color = 0xff24546au;
constexpr u32 taskbar_launcher_color = 0xff152536u;
constexpr u32 taskbar_launcher_open_color = 0xff1e4256u;
constexpr u32 taskbar_launcher_active_color = 0xff276078u;
constexpr u32 taskbar_icon_foreground = 0xffd8f3ffu;
constexpr u32 shell_launcher_x = 6;
constexpr u32 file_manager_launcher_x = shell_launcher_x + taskbar_icon_size + 6;
constexpr u32 title_hit_height = gui_glyph_height * 2 + 4;
constexpr u32 resize_hit_size = 8;
constexpr u32 minimum_surface_width = 40;
constexpr u32 minimum_surface_height = 28;
constexpr i32 window_control_top = 3;
constexpr i32 window_control_bottom = 8;
constexpr i32 window_close_right_min = 8;
constexpr i32 window_close_right_max = 13;
constexpr i32 window_maximize_right_min = 17;
constexpr i32 window_maximize_right_max = 22;
constexpr i32 window_minimize_right_min = 26;
constexpr i32 window_minimize_right_max = 31;

enum class WindowControl : u8
{
    none,
    minimize,
    maximize,
    close,
};

[[nodiscard]] i32 min_i32(i32 left, i32 right)
{
    return left < right ? left : right;
}

[[nodiscard]] i32 max_i32(i32 left, i32 right)
{
    return left > right ? left : right;
}

[[nodiscard]] i32 clamp_i32(i32 value, i32 low, i32 high)
{
    if (value < low)
    {
        return low;
    }
    if (value > high)
    {
        return high;
    }
    return value;
}

[[nodiscard]] u32 clamp_u32(u32 value, u32 low, u32 high)
{
    if (value < low)
    {
        return low;
    }
    if (value > high)
    {
        return high;
    }
    return value;
}

[[nodiscard]] WindowControl window_control_at(const SurfaceInfo &surface, i32 local_x, i32 local_y)
{
    if (local_x < 0 || local_y < window_control_top || local_y > window_control_bottom || surface.bounds.width < 34)
    {
        return WindowControl::none;
    }
    const auto right = static_cast<i32>(surface.bounds.width) - local_x;
    if (right >= window_close_right_min && right <= window_close_right_max)
    {
        return WindowControl::close;
    }
    if (right >= window_maximize_right_min && right <= window_maximize_right_max)
    {
        return WindowControl::maximize;
    }
    if (right >= window_minimize_right_min && right <= window_minimize_right_max)
    {
        return WindowControl::minimize;
    }
    return WindowControl::none;
}

[[nodiscard]] bool resize_hit(const SurfaceInfo &surface, i32 local_x, i32 local_y)
{
    if (surface.bounds.width < resize_hit_size || surface.bounds.height < resize_hit_size)
    {
        return false;
    }
    return local_x >= static_cast<i32>(surface.bounds.width - resize_hit_size) &&
           local_y >= static_cast<i32>(surface.bounds.height - resize_hit_size);
}

[[nodiscard]] Rect minimized_bounds_for_index(usize index, Rect desktop)
{
    constexpr u32 width = 112;
    constexpr u32 height = taskbar_height > 4 ? taskbar_height - 4 : taskbar_height;
    const auto x = static_cast<i32>(taskbar_launcher_width + 4 + (index % max_gui_surfaces) * (width + 4));
    const auto y = static_cast<i32>(desktop.height > taskbar_height ? desktop.height - taskbar_height + 2 : 0);
    u32 clipped_width = width;
    if (x >= 0 && static_cast<u32>(x) + clipped_width > desktop.width)
    {
        clipped_width = desktop.width > static_cast<u32>(x) ? desktop.width - static_cast<u32>(x) : width;
    }
    return Rect{.x = x, .y = y, .width = clipped_width, .height = height};
}

[[nodiscard]] bool point_in_rect(u32 x, u32 y, u32 left, u32 top, u32 width, u32 height)
{
    return x >= left && y >= top && x < left + width && y < top + height;
}

[[nodiscard]] bool point_in_gui_rect(i32 x, i32 y, Rect bounds)
{
    return x >= bounds.x && y >= bounds.y && x < bounds.x + static_cast<i32>(bounds.width) &&
           y < bounds.y + static_cast<i32>(bounds.height);
}

[[nodiscard]] bool shell_icon_pixel(u32 local_x, u32 local_y)
{
    if (local_x == 3 || local_x == 14 || local_y == 3 || local_y == 14)
    {
        return local_x >= 3 && local_x <= 14 && local_y >= 3 && local_y <= 14;
    }
    const bool prompt_top = local_x >= 6 && local_x <= 8 && local_y == 7;
    const bool prompt_mid = local_x >= 8 && local_x <= 10 && local_y == 8;
    const bool prompt_bottom = local_x >= 6 && local_x <= 8 && local_y == 9;
    const bool cursor = local_x >= 10 && local_x <= 13 && local_y == 11;
    return prompt_top || prompt_mid || prompt_bottom || cursor;
}

[[nodiscard]] bool file_manager_icon_pixel(u32 local_x, u32 local_y)
{
    const bool tab = local_x >= 3 && local_x <= 8 && local_y >= 4 && local_y <= 6;
    const bool body = local_x >= 3 && local_x <= 15 && local_y >= 7 && local_y <= 14;
    const bool border = (local_y == 7 && local_x >= 3 && local_x <= 15) ||
                        (local_y == 14 && local_x >= 3 && local_x <= 15) ||
                        (local_x == 3 && local_y >= 7 && local_y <= 14) ||
                        (local_x == 15 && local_y >= 7 && local_y <= 14);
    const bool handle = local_x >= 6 && local_x <= 12 && local_y == 10;
    return tab || border || handle || (body && ((local_x + local_y) % 7u == 0));
}

[[nodiscard]] u32 taskbar_launcher_pixel_color(u32 height, u32 x, u32 y, TaskbarApp app, bool open, bool active)
{
    const auto taskbar_top = height > taskbar_height ? height - taskbar_height : 0;
    if (y < taskbar_top + 3 || y >= taskbar_top + 3 + taskbar_icon_size)
    {
        return 0;
    }

    const u32 left = app == TaskbarApp::debug_shell ? shell_launcher_x : file_manager_launcher_x;
    if (!point_in_rect(x, y, left, taskbar_top + 3, taskbar_icon_size, taskbar_icon_size))
    {
        return 0;
    }

    const auto local_x = x - left;
    const auto local_y = y - (taskbar_top + 3);
    u32 color = active ? taskbar_launcher_active_color : (open ? taskbar_launcher_open_color : taskbar_launcher_color);
    if (local_x == 0 || local_y == 0 || local_x + 1 == taskbar_icon_size || local_y + 1 == taskbar_icon_size)
    {
        color = open ? taskbar_edge_color : 0xff40566au;
    }
    const bool icon_pixel = app == TaskbarApp::debug_shell ? shell_icon_pixel(local_x, local_y)
                                                           : file_manager_icon_pixel(local_x, local_y);
    if (icon_pixel)
    {
        color = app == TaskbarApp::debug_shell ? taskbar_icon_foreground : 0xffffcc66u;
    }
    if (open && local_y + 2 >= taskbar_icon_size && local_x >= 5 && local_x <= 12)
    {
        color = active ? 0xffffcc66u : taskbar_edge_color;
    }
    return color;
}

[[nodiscard]] u32 desktop_pixel_color(u32 width, u32 height, u32 x, u32 y)
{
    const auto taskbar_top = height > taskbar_height ? height - taskbar_height : 0;
    if (y >= taskbar_top)
    {
        if (y == taskbar_top)
        {
            return taskbar_edge_color;
        }
        if ((x % 48u) == 0)
        {
            return 0xff152636u;
        }
        return taskbar_color;
    }

    const auto ix = static_cast<i32>(x);
    const auto iy = static_cast<i32>(y);
    const auto center_y = static_cast<i32>(height / 2);
    const auto o_center_x = static_cast<i32>(width / 2) - 86;
    const auto k_left = static_cast<i32>(width / 2) + 16;

    constexpr i32 outer_radius = 58;
    constexpr i32 inner_radius = 34;
    const auto odx = ix - o_center_x;
    const auto ody = iy - center_y;
    const auto distance = odx * odx + ody * ody;
    const bool in_o = distance <= outer_radius * outer_radius && distance >= inner_radius * inner_radius;

    const bool k_stem = ix >= k_left && ix <= k_left + 13 && iy >= center_y - 58 && iy <= center_y + 58;
    const auto k_mid_x = k_left + 20;
    const auto k_mid_y = center_y;
    const bool k_upper = ix >= k_mid_x && ix <= k_mid_x + 80 && iy <= k_mid_y &&
                         (iy >= k_mid_y - (ix - k_mid_x) - 13) && (iy <= k_mid_y - (ix - k_mid_x) + 13);
    const bool k_lower = ix >= k_mid_x && ix <= k_mid_x + 80 && iy >= k_mid_y &&
                         (iy >= k_mid_y + (ix - k_mid_x) - 13) && (iy <= k_mid_y + (ix - k_mid_x) + 13);

    if (in_o || k_stem || k_upper || k_lower)
    {
        return ((x + y) % 17u) < 4u ? 0xfff4d35eu : 0xff1d8f83u;
    }
    if ((y % 24u) == 0 || (x % 64u) == 0)
    {
        return 0xff07151cu;
    }
    return desktop_background_color;
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
    pointer_x_ = static_cast<i32>(display.mode().width / 2);
    pointer_y_ = static_cast<i32>(display.mode().height / 2);
    left_button_down_ = false;
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
    active_surface_id_ = 0;
    next_z_index_ = 1;
    dragging_surface_id_ = 0;
    resizing_surface_id_ = 0;
    pending_window_event_ = {};
}

Rect GuiCompositor::work_area_bounds() const
{
    const auto mode = display_->mode();
    const auto height = mode.height > taskbar_height ? mode.height - taskbar_height : mode.height;
    return Rect{.x = 0, .y = 0, .width = mode.width, .height = height};
}

bool GuiCompositor::app_window_exists(TaskbarApp app) const
{
    if (app == TaskbarApp::none)
    {
        return false;
    }
    for (const auto &surface : surfaces_)
    {
        if (surface.app == app)
        {
            return true;
        }
    }
    return false;
}

TaskbarApp GuiCompositor::active_app() const
{
    for (const auto &surface : surfaces_)
    {
        if (surface.id == active_surface_id_)
        {
            return surface.app;
        }
    }
    return TaskbarApp::none;
}

void GuiCompositor::focus_top_surface()
{
    const Surface *top = nullptr;
    for (const auto &surface : surfaces_)
    {
        if (!surface.visible || surface.window_state == WindowState::minimized)
        {
            continue;
        }
        if (top == nullptr || surface.z_index > top->z_index)
        {
            top = &surface;
        }
    }
    active_surface_id_ = top == nullptr ? SurfaceId{0} : top->id;
}

void GuiCompositor::record_window_event(WindowEventKind kind, SurfaceId id)
{
    pending_window_event_ = WindowEvent{.kind = kind, .surface_id = id};
    if (auto info = surface_info(id))
    {
        pending_window_event_.bounds = info.value().bounds;
    }
}

WindowEvent GuiCompositor::consume_window_event()
{
    const auto event = pending_window_event_;
    pending_window_event_ = {};
    return event;
}

Result<TaskbarApp> GuiCompositor::taskbar_launcher_at(i32 x, i32 y) const
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    if (x < 0 || y < 0)
    {
        return TaskbarApp::none;
    }
    const auto mode = display_->mode();
    const auto taskbar_top = mode.height > taskbar_height ? mode.height - taskbar_height : 0;
    const auto ux = static_cast<u32>(x);
    const auto uy = static_cast<u32>(y);
    if (uy < taskbar_top + 3 || uy >= taskbar_top + 3 + taskbar_icon_size)
    {
        return TaskbarApp::none;
    }
    if (point_in_rect(ux, uy, shell_launcher_x, taskbar_top + 3, taskbar_icon_size, taskbar_icon_size))
    {
        return TaskbarApp::debug_shell;
    }
    if (point_in_rect(ux, uy, file_manager_launcher_x, taskbar_top + 3, taskbar_icon_size, taskbar_icon_size))
    {
        return TaskbarApp::file_manager;
    }
    return TaskbarApp::none;
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
    slot.restore_bounds = bounds;
    slot.z_index = next_z_index_;
    slot.visible = true;
    slot.window_state = WindowState::normal;
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
    active_surface_id_ = id;
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
    if (dragging_surface_id_ == id)
    {
        dragging_surface_id_ = 0;
    }
    if (resizing_surface_id_ == id)
    {
        resizing_surface_id_ = 0;
    }
    if (active_surface_id_ == id)
    {
        active_surface_id_ = 0;
    }
    if (auto status = surfaces_.erase_at(index.value()); !status.ok())
    {
        return status;
    }
    if (active_surface_id_ == 0)
    {
        focus_top_surface();
    }
    return Status::success();
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
    auto &surface = surfaces_[index.value()];
    surface.visible = visible;
    if (!visible && active_surface_id_ == id)
    {
        active_surface_id_ = 0;
        focus_top_surface();
    }
    else if (visible && surface.window_state != WindowState::minimized)
    {
        active_surface_id_ = id;
    }
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

Status GuiCompositor::set_surface_app(SurfaceId id, TaskbarApp app)
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
    surfaces_[index.value()].app = app;
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
    if (surface.window_state == WindowState::normal)
    {
        surface.restore_bounds = surface.bounds;
    }
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
    if (surface.window_state == WindowState::normal)
    {
        surface.restore_bounds = bounds;
    }
    record_window_event(WindowEventKind::resized, id);
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
    auto &surface = surfaces_[index.value()];
    surface.z_index = next_z_index_;
    ++next_z_index_;
    if (surface.visible && surface.window_state != WindowState::minimized)
    {
        active_surface_id_ = id;
    }
    return Status::success();
}

Status GuiCompositor::minimize_surface(SurfaceId id)
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
    if (surface.window_state == WindowState::normal)
    {
        surface.restore_bounds = surface.bounds;
    }
    auto desktop = desktop_bounds();
    if (!desktop)
    {
        return desktop.status();
    }
    surface.bounds = minimized_bounds_for_index(index.value(), desktop.value());
    surface.visible = true;
    surface.window_state = WindowState::minimized;
    surface.z_index = next_z_index_;
    ++next_z_index_;
    if (active_surface_id_ == id)
    {
        active_surface_id_ = 0;
        focus_top_surface();
    }
    record_window_event(WindowEventKind::minimized, id);
    return Status::success();
}

Status GuiCompositor::maximize_surface(SurfaceId id)
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
    if (surface.window_state == WindowState::normal)
    {
        surface.restore_bounds = surface.bounds;
    }
    surface.bounds = work_area_bounds();
    surface.visible = true;
    surface.window_state = WindowState::maximized;
    if (auto status = raise_surface(id); !status.ok())
    {
        return status;
    }
    record_window_event(WindowEventKind::maximized, id);
    return Status::success();
}

Status GuiCompositor::restore_surface(SurfaceId id)
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
    if (surface.window_state == WindowState::maximized || surface.window_state == WindowState::minimized)
    {
        surface.bounds = surface.restore_bounds;
    }
    surface.visible = true;
    surface.window_state = WindowState::normal;
    if (auto status = raise_surface(id); !status.ok())
    {
        return status;
    }
    record_window_event(WindowEventKind::restored, id);
    return Status::success();
}

Status GuiCompositor::close_surface(SurfaceId id)
{
    return destroy_surface(id);
}

Status GuiCompositor::set_pointer_position(i32 x, i32 y)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    const auto mode = display_->mode();
    pointer_x_ = clamp_i32(x, 0, static_cast<i32>(mode.width) - 1);
    pointer_y_ = clamp_i32(y, 0, static_cast<i32>(mode.height) - 1);
    return Status::success();
}

Status GuiCompositor::handle_mouse_delta(i32 delta_x, i32 delta_y, bool left_button)
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    const auto mode = display_->mode();
    pointer_x_ = clamp_i32(pointer_x_ + delta_x, 0, static_cast<i32>(mode.width) - 1);
    pointer_y_ = clamp_i32(pointer_y_ + delta_y, 0, static_cast<i32>(mode.height) - 1);
    const auto taskbar_top = mode.height > taskbar_height ? static_cast<i32>(mode.height - taskbar_height) : 0;

    const bool pressed = left_button && !left_button_down_;
    const bool released = !left_button && left_button_down_;
    const bool pointer_changed = delta_x != 0 || delta_y != 0 || pressed || released;
    bool changed = false;

    if (released)
    {
        dragging_surface_id_ = 0;
        resizing_surface_id_ = 0;
    }

    if (pressed)
    {
        auto hit = surface_at(pointer_x_, pointer_y_);
        if (hit)
        {
            auto info = surface_info(hit.value());
            if (!info)
            {
                return info.status();
            }
            if (auto status = raise_surface(hit.value()); !status.ok())
            {
                return status;
            }
            changed = true;

            if (pointer_y_ >= taskbar_top)
            {
                dragging_surface_id_ = 0;
                resizing_surface_id_ = 0;
                if (info.value().window_state == WindowState::minimized)
                {
                    if (auto status = restore_surface(hit.value()); !status.ok())
                    {
                        return status;
                    }
                }
                left_button_down_ = left_button;
                return present();
            }

            const auto local_x = pointer_x_ - info.value().bounds.x;
            const auto local_y = pointer_y_ - info.value().bounds.y;
            if (info.value().window_state == WindowState::minimized)
            {
                dragging_surface_id_ = 0;
                resizing_surface_id_ = 0;
                if (auto status = restore_surface(hit.value()); !status.ok())
                {
                    return status;
                }
                left_button_down_ = left_button;
                return present();
            }
            switch (window_control_at(info.value(), local_x, local_y))
            {
            case WindowControl::close:
                dragging_surface_id_ = 0;
                resizing_surface_id_ = 0;
                record_window_event(WindowEventKind::close_request, hit.value());
                left_button_down_ = left_button;
                return present();
            case WindowControl::maximize:
                dragging_surface_id_ = 0;
                resizing_surface_id_ = 0;
                if (info.value().window_state == WindowState::maximized)
                {
                    if (auto status = restore_surface(hit.value()); !status.ok())
                    {
                        return status;
                    }
                }
                else
                {
                    if (auto status = maximize_surface(hit.value()); !status.ok())
                    {
                        return status;
                    }
                }
                left_button_down_ = left_button;
                return present();
            case WindowControl::minimize:
                dragging_surface_id_ = 0;
                resizing_surface_id_ = 0;
                if (info.value().window_state == WindowState::maximized)
                {
                    if (auto status = restore_surface(hit.value()); !status.ok())
                    {
                        return status;
                    }
                }
                else if (auto status = minimize_surface(hit.value()); !status.ok())
                {
                    return status;
                }
                left_button_down_ = left_button;
                return present();
            case WindowControl::none:
                break;
            }

            if (info.value().window_state == WindowState::normal && resize_hit(info.value(), local_x, local_y))
            {
                resizing_surface_id_ = hit.value();
            }
            else if (info.value().window_state == WindowState::normal && local_y >= 0 &&
                     local_y < static_cast<i32>(title_hit_height))
            {
                dragging_surface_id_ = hit.value();
                drag_offset_x_ = local_x;
                drag_offset_y_ = local_y;
            }
        }
    }

    if (left_button && dragging_surface_id_ != 0)
    {
        auto info = surface_info(dragging_surface_id_);
        if (info)
        {
            if (auto status =
                    move_surface(dragging_surface_id_, pointer_x_ - drag_offset_x_, pointer_y_ - drag_offset_y_);
                !status.ok())
            {
                return status;
            }
            changed = true;
        }
        else
        {
            dragging_surface_id_ = 0;
        }
    }

    if (left_button && resizing_surface_id_ != 0)
    {
        auto info = surface_info(resizing_surface_id_);
        if (info)
        {
            auto bounds = info.value().bounds;
            const auto requested_width = static_cast<u32>(max_i32(pointer_x_ - bounds.x + 1,
                                                                  static_cast<i32>(minimum_surface_width)));
            const auto requested_height = static_cast<u32>(max_i32(pointer_y_ - bounds.y + 1,
                                                                   static_cast<i32>(minimum_surface_height)));
            bounds.width = clamp_u32(requested_width, minimum_surface_width, max_gui_surface_width);
            bounds.height = clamp_u32(requested_height, minimum_surface_height, max_gui_surface_height);
            if (auto status = resize_surface(resizing_surface_id_, bounds); !status.ok())
            {
                return status;
            }
            changed = true;
        }
        else
        {
            resizing_surface_id_ = 0;
        }
    }

    left_button_down_ = left_button;
    return (changed || pointer_changed) ? present() : Status::success();
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

u32 GuiCompositor::taskbar_surface_pixel_color(const Surface &surface, u32 x, u32 y, u32 width, u32 height) const
{
    u32 color = surface.id == active_surface_id_ ? active_taskbar_button_color : taskbar_button_color;
    if (x == 0 || y == 0 || x + 1 == width || y + 1 == height)
    {
        color = surface.id == active_surface_id_ ? frame_color : inactive_title_frame_color;
    }
    else if (y == 1)
    {
        color = taskbar_edge_color;
    }
    const auto text_x = x >= 8 ? x - 8 : width;
    const auto text_y = y >= 7 ? y - 7 : height;
    const auto char_index = text_x / gui_glyph_width;
    const auto glyph_x = text_x % gui_glyph_width;
    if (char_index < surface.title.size() && glyph_x < driver::BitmapFontRenderer::glyph_width &&
        text_y < driver::BitmapFontRenderer::glyph_height)
    {
        const auto row_bits = driver::BitmapFontRenderer::glyph_row(surface.title.view()[char_index], text_y);
        if (((row_bits >> (driver::BitmapFontRenderer::glyph_width - 1 - glyph_x)) & 1u) != 0)
        {
            color = 0xffd8f3ffu;
        }
    }
    return color;
}

u32 GuiCompositor::surface_pixel_color(const Surface &surface, u32 x, u32 y) const
{
    if (surface.window_state == WindowState::minimized)
    {
        return taskbar_surface_pixel_color(surface, x, y, surface.bounds.width, surface.bounds.height);
    }

    auto color = surface.pixels[static_cast<usize>(y) * max_gui_surface_width + x];
    if (x == 0 || y == 0 || x + 1 == surface.bounds.width || y + 1 == surface.bounds.height)
    {
        color = frame_color;
    }
    else if (y == 1)
    {
        color = surface.id == active_surface_id_ ? title_frame_color : inactive_title_frame_color;
    }
    else if (y == 2)
    {
        color = surface.id == active_surface_id_ ? title_color : inactive_title_color;
    }
    else if (y >= static_cast<u32>(window_control_top) && y <= static_cast<u32>(window_control_bottom) &&
             surface.bounds.width >= 34)
    {
        const auto right = static_cast<i32>(surface.bounds.width) - static_cast<i32>(x);
        if (right >= window_close_right_min && right <= window_close_right_max)
        {
            color = 0xffd85f5fu;
        }
        else if (right >= window_maximize_right_min && right <= window_maximize_right_max)
        {
            color = surface.window_state == WindowState::maximized ? 0xfff4d35eu : 0xff8fd7ffu;
        }
        else if (right >= window_minimize_right_min && right <= window_minimize_right_max)
        {
            color = 0xff8fbf88u;
        }
    }
    return color;
}

Status GuiCompositor::present()
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }

    const auto mode = display_->mode();
    const auto taskbar_top = mode.height > taskbar_height ? mode.height - taskbar_height : 0;
    const bool shell_open = app_window_exists(TaskbarApp::debug_shell);
    const bool files_open = app_window_exists(TaskbarApp::file_manager);
    const auto focused_app = active_app();
    for (u32 y = 0; y < mode.height; ++y)
    {
        for (u32 x = 0; x < mode.width; ++x)
        {
            auto color = desktop_pixel_color(mode.width, mode.height, x, y);
            if (const auto shell_color = taskbar_launcher_pixel_color(mode.height, x, y, TaskbarApp::debug_shell,
                                                                       shell_open,
                                                                       focused_app == TaskbarApp::debug_shell);
                shell_color != 0)
            {
                color = shell_color;
            }
            if (const auto files_color = taskbar_launcher_pixel_color(mode.height, x, y, TaskbarApp::file_manager,
                                                                       files_open,
                                                                       focused_app == TaskbarApp::file_manager);
                files_color != 0)
            {
                color = files_color;
            }
            if (y >= taskbar_top)
            {
                const Rect desktop{.x = 0, .y = 0, .width = mode.width, .height = mode.height};
                for (usize i = 0; i < surfaces_.size(); ++i)
                {
                    const auto &surface = surfaces_[i];
                    if (!surface.visible)
                    {
                        continue;
                    }
                    const auto bounds = minimized_bounds_for_index(i, desktop);
                    if (!point_in_gui_rect(static_cast<i32>(x), static_cast<i32>(y), bounds))
                    {
                        continue;
                    }
                    color = taskbar_surface_pixel_color(surface, x - static_cast<u32>(bounds.x),
                                                        y - static_cast<u32>(bounds.y), bounds.width, bounds.height);
                    break;
                }
            }
            const Surface *top = nullptr;
            u32 surface_x = 0;
            u32 surface_y = 0;
            for (const auto &surface : surfaces_)
            {
                if (!surface.visible)
                {
                    continue;
                }
                if (y >= taskbar_top)
                {
                    continue;
                }
                const auto local_x = static_cast<i32>(x) - surface.bounds.x;
                const auto local_y = static_cast<i32>(y) - surface.bounds.y;
                if (local_x < 0 || local_y < 0 || local_x >= static_cast<i32>(surface.bounds.width) ||
                    local_y >= static_cast<i32>(surface.bounds.height))
                {
                    continue;
                }
                if (top == nullptr || surface.z_index > top->z_index)
                {
                    top = &surface;
                    surface_x = static_cast<u32>(local_x);
                    surface_y = static_cast<u32>(local_y);
                }
            }
            if (top != nullptr)
            {
                color = surface_pixel_color(*top, surface_x, surface_y);
            }
            frame_pixels_[static_cast<usize>(y) * mode.width + x] = color;
        }
    }

    if (auto status = display_->present_gui_frame(
            mode.width, mode.height,
            std::span<const u32>(frame_pixels_.data(), static_cast<usize>(mode.width) * mode.height));
        !status.ok())
    {
        return status;
    }
    last_present_checksum_ = display_->checksum();
    return Status::success();
}

Status GuiCompositor::play_startup_animation()
{
    auto desktop = desktop_bounds();
    if (!desktop)
    {
        return desktop.status();
    }

    auto surface = create_surface(desktop.value(), "okernel");
    if (!surface)
    {
        return surface.status();
    }
    const auto id = surface.value();

    constexpr u32 frames = 12;
    for (u32 frame = 0; frame < frames; ++frame)
    {
        if (auto status = fill(id, 0xff071018u); !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }

        const auto pulse = static_cast<i32>((frame * 17u) % 96u);
        if (auto status =
                fill_rect(id, Rect{.x = 0, .y = 0, .width = desktop.value().width, .height = desktop.value().height},
                          0xff071018u);
            !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }
        if (auto status = fill_rect(id, Rect{.x = 28 + pulse, .y = 46, .width = 116, .height = 9}, 0xff44aa88u);
            !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }
        if (auto status = fill_rect(id, Rect{.x = 328 - pulse, .y = 206, .width = 96, .height = 7}, 0xffffcc66u);
            !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }
        if (auto status = fill_rect(id, Rect{.x = 92, .y = 82, .width = 296, .height = 86}, title_color); !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }
        if (auto status = fill_rect(id, Rect{.x = 102, .y = 92, .width = 276, .height = 3}, 0xffd8f3ffu);
            !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }
        if (auto status = fill_rect(id, Rect{.x = 102, .y = 151, .width = 276, .height = 3}, 0xff44aa88u);
            !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }

        const auto progress_width = 28u + frame * 20u;
        if (auto status = fill_rect(id, Rect{.x = 116, .y = 137, .width = progress_width, .height = 6}, 0xffffcc66u);
            !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }
        if (auto status = draw_text(id, 19, 11, "OBFUSCATION KERNEL", 0xffd8f3ffu, title_color); !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }
        if (auto status = draw_text(id, 23, 13, "GUI ONLINE", 0xffffcc66u, title_color); !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }
        if (auto status = present(); !status.ok())
        {
            static_cast<void>(destroy_surface(id));
            return status;
        }
        ++startup_animation_frames_;
    }

    return destroy_surface(id);
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
        .focused = surface.id == active_surface_id_,
        .window_state = surface.window_state,
        .app = surface.app,
        .title = surface.title.view(),
    };
}

Result<SurfaceId> GuiCompositor::taskbar_surface_at(i32 x, i32 y) const
{
    auto desktop = desktop_bounds();
    if (!desktop)
    {
        return desktop.status();
    }
    for (usize i = 0; i < surfaces_.size(); ++i)
    {
        const auto &surface = surfaces_[i];
        if (!surface.visible)
        {
            continue;
        }
        if (point_in_gui_rect(x, y, minimized_bounds_for_index(i, desktop.value())))
        {
            return surface.id;
        }
    }
    return Status::not_found("no GUI taskbar surface at point");
}

Result<SurfaceId> GuiCompositor::surface_at(i32 x, i32 y) const
{
    if (auto status = ensure_running(); !status.ok())
    {
        return status;
    }
    const auto mode = display_->mode();
    const auto taskbar_top = mode.height > taskbar_height ? static_cast<i32>(mode.height - taskbar_height) : 0;
    if (y >= taskbar_top)
    {
        return taskbar_surface_at(x, y);
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
    const auto threading =
        display_ != nullptr && display_->uses_cpu_gui_render_workers() ? ModuleThreading::per_cpu
                                                                       : ModuleThreading::single_threaded;
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
        .threading = threading,
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
