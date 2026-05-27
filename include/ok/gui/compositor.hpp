#pragma once

#include "ok/core/fixed.hpp"
#include "ok/driver/driver.hpp"
#include "ok/driver/font.hpp"

#include <array>
#include <string_view>

namespace ok::gui
{

inline constexpr std::string_view gui_module_name{"kernel-gui"};
inline constexpr std::string_view gui_service_id{"gui.compositor"};
inline constexpr usize max_gui_surfaces = 8;
inline constexpr usize max_gui_title = 32;
inline constexpr usize max_gui_crash_reason = 64;
inline constexpr u32 max_gui_surface_width = driver::framebuffer_width;
inline constexpr u32 max_gui_surface_height = driver::framebuffer_height;
inline constexpr usize max_gui_surface_pixels = max_gui_surface_width * max_gui_surface_height;
inline constexpr u32 gui_glyph_width = driver::BitmapFontRenderer::glyph_width + 1;
inline constexpr u32 gui_glyph_height = driver::BitmapFontRenderer::glyph_height + 2;
inline constexpr u32 taskbar_height = gui_glyph_height * 2 + 6;
inline constexpr u32 taskbar_icon_size = 18;
inline constexpr u32 taskbar_launcher_width = 84;
inline constexpr u32 task_monitor_launcher_x = 54;

using SurfaceId = u16;

struct Rect
{
    i32 x{0};
    i32 y{0};
    u32 width{0};
    u32 height{0};
};

enum class GuiState : u8
{
    stopped,
    running,
    crashed,
};

enum class WindowState : u8
{
    normal,
    minimized,
    maximized,
};

enum class WindowEventKind : u8
{
    none,
    close_request,
    resized,
    minimized,
    maximized,
    restored,
};

enum class TaskbarApp : u8
{
    none,
    debug_shell,
    file_manager,
    task_monitor,
};

enum class ScrollDirection : u8
{
    previous,
    next,
};

struct ScrollCommand
{
    ScrollDirection direction{ScrollDirection::next};
    usize rows{0};
};

[[nodiscard]] constexpr usize scroll_magnitude(i32 rows)
{
    return rows > 0 ? static_cast<usize>(rows) : static_cast<usize>(-(rows + 1)) + static_cast<usize>(1);
}

[[nodiscard]] constexpr ScrollCommand scroll_command_from_rows(i32 rows)
{
    return ScrollCommand{
        .direction = rows > 0 ? ScrollDirection::previous : ScrollDirection::next,
        .rows = rows == 0 ? static_cast<usize>(0) : scroll_magnitude(rows),
    };
}

[[nodiscard]] constexpr i32 scroll_rows(ScrollDirection direction, usize rows = 1)
{
    const auto value = static_cast<i32>(rows);
    return direction == ScrollDirection::previous ? value : -value;
}

struct SurfaceInfo
{
    SurfaceId id{0};
    Rect bounds{};
    u32 z_index{0};
    bool visible{false};
    bool focused{false};
    WindowState window_state{WindowState::normal};
    TaskbarApp app{TaskbarApp::none};
    std::string_view title{};
};

struct WindowEvent
{
    WindowEventKind kind{WindowEventKind::none};
    SurfaceId surface_id{0};
    Rect bounds{};
};

class GuiCompositor final
{
  public:
    [[nodiscard]] std::string_view service_id() const
    {
        return gui_service_id;
    }

    Status start(driver::FramebufferDisplayDriver &display);
    Status stop();
    Status simulate_crash(std::string_view reason);
    [[nodiscard]] bool crashed() const
    {
        return state_ == GuiState::crashed;
    }

    Result<SurfaceId> create_surface(Rect bounds, std::string_view title);
    Status destroy_surface(SurfaceId id);
    Status set_visible(SurfaceId id, bool visible);
    Status set_title(SurfaceId id, std::string_view title);
    Status set_surface_app(SurfaceId id, TaskbarApp app);
    Status move_surface(SurfaceId id, i32 x, i32 y);
    Status resize_surface(SurfaceId id, Rect bounds);
    Status raise_surface(SurfaceId id);
    Status minimize_surface(SurfaceId id);
    Status maximize_surface(SurfaceId id);
    Status restore_surface(SurfaceId id);
    Status close_surface(SurfaceId id);
    Status handle_mouse_delta(i32 delta_x, i32 delta_y, bool left_button);
    Status set_pointer_position(i32 x, i32 y);
    Status fill(SurfaceId id, u32 rgba);
    Status fill_rect(SurfaceId id, Rect rect, u32 rgba);
    Status put_pixel(SurfaceId id, u32 x, u32 y, u32 rgba);
    Status draw_text(SurfaceId id, u32 column, u32 row, std::string_view text, u32 foreground, u32 background);
    Status present();
    Status play_startup_animation();

    [[nodiscard]] GuiState state() const
    {
        return state_;
    }
    [[nodiscard]] usize surface_count() const
    {
        return surfaces_.size();
    }
    [[nodiscard]] u64 generation() const
    {
        return generation_;
    }
    [[nodiscard]] u64 last_present_checksum() const
    {
        return last_present_checksum_;
    }
    [[nodiscard]] usize startup_animation_frames() const
    {
        return startup_animation_frames_;
    }
    [[nodiscard]] std::string_view crash_reason() const
    {
        return crash_reason_.view();
    }
    [[nodiscard]] Result<SurfaceInfo> surface_info(SurfaceId id) const;
    [[nodiscard]] Result<SurfaceId> surface_at(i32 x, i32 y) const;
    [[nodiscard]] Result<Rect> desktop_bounds() const;
    [[nodiscard]] WindowEvent consume_window_event();
    [[nodiscard]] Result<TaskbarApp> taskbar_launcher_at(i32 x, i32 y) const;
    [[nodiscard]] SurfaceId active_surface() const
    {
        return active_surface_id_;
    }
    [[nodiscard]] i32 pointer_x() const
    {
        return pointer_x_;
    }
    [[nodiscard]] i32 pointer_y() const
    {
        return pointer_y_;
    }

  private:
    struct Surface
    {
        SurfaceId id{0};
        FixedString<max_gui_title> title{};
        Rect bounds{};
        Rect restore_bounds{};
        u32 z_index{0};
        bool visible{true};
        WindowState window_state{WindowState::normal};
        TaskbarApp app{TaskbarApp::none};
        std::array<u32, max_gui_surface_pixels> pixels{};
    };

    [[nodiscard]] Status ensure_running() const;
    [[nodiscard]] Result<usize> find_surface_index(SurfaceId id) const;
    [[nodiscard]] Status validate_bounds(Rect bounds) const;
    void draw_cell(Surface &surface, u32 column, u32 row, char value, u32 foreground, u32 background);
    [[nodiscard]] u32 surface_pixel_color(const Surface &surface, u32 x, u32 y) const;
    [[nodiscard]] u32 taskbar_surface_pixel_color(const Surface &surface, u32 x, u32 y, u32 width, u32 height) const;
    [[nodiscard]] Result<SurfaceId> taskbar_surface_at(i32 x, i32 y) const;
    [[nodiscard]] Rect work_area_bounds() const;
    [[nodiscard]] bool app_window_exists(TaskbarApp app) const;
    [[nodiscard]] TaskbarApp active_app() const;
    void focus_top_surface();
    void record_window_event(WindowEventKind kind, SurfaceId id);
    void reset_surfaces();

    driver::FramebufferDisplayDriver *display_{nullptr};
    StaticVector<Surface, max_gui_surfaces> surfaces_{};
    std::array<u32, max_gui_surface_pixels> frame_pixels_{};
    GuiState state_{GuiState::stopped};
    SurfaceId next_surface_id_{1};
    SurfaceId active_surface_id_{0};
    u32 next_z_index_{1};
    u64 generation_{0};
    u64 last_present_checksum_{0};
    usize startup_animation_frames_{0};
    FixedString<max_gui_crash_reason> crash_reason_{};
    i32 pointer_x_{0};
    i32 pointer_y_{0};
    bool left_button_down_{false};
    SurfaceId dragging_surface_id_{0};
    SurfaceId resizing_surface_id_{0};
    WindowEvent pending_window_event_{};
    i32 drag_offset_x_{0};
    i32 drag_offset_y_{0};
};

[[nodiscard]] std::string_view gui_state_name(GuiState state);

} // namespace ok::gui
