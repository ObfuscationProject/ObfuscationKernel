#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/module.hpp"
#include "ok/driver/driver.hpp"
#include "ok/driver/font.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/user/user.hpp"

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

struct SurfaceInfo
{
    SurfaceId id{0};
    Rect bounds{};
    u32 z_index{0};
    bool visible{false};
    WindowState window_state{WindowState::normal};
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
        std::array<u32, max_gui_surface_pixels> pixels{};
    };

    [[nodiscard]] Status ensure_running() const;
    [[nodiscard]] Result<usize> find_surface_index(SurfaceId id) const;
    [[nodiscard]] Status validate_bounds(Rect bounds) const;
    void draw_cell(Surface &surface, u32 column, u32 row, char value, u32 foreground, u32 background);
    [[nodiscard]] u32 surface_pixel_color(const Surface &surface, u32 x, u32 y) const;
    void record_window_event(WindowEventKind kind, SurfaceId id);
    void reset_surfaces();

    driver::FramebufferDisplayDriver *display_{nullptr};
    StaticVector<Surface, max_gui_surfaces> surfaces_{};
    GuiState state_{GuiState::stopped};
    SurfaceId next_surface_id_{1};
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

class KernelFileManager final
{
  public:
    Status open(GuiCompositor &compositor, fs::VirtualFileSystem &vfs, std::string_view path,
                user::Credentials credentials, sched::ProcessId process_id);
    Status refresh(GuiCompositor &compositor, fs::VirtualFileSystem &vfs);
    Status close(GuiCompositor &compositor);
    void mark_closed();
    Status handle_surface_changed(GuiCompositor &compositor, fs::VirtualFileSystem &vfs);
    Status handle_mouse(GuiCompositor &compositor, fs::VirtualFileSystem &vfs, i32 x, i32 y, bool click);

    [[nodiscard]] SurfaceId surface_id() const
    {
        return surface_id_;
    }
    [[nodiscard]] std::string_view path() const
    {
        return path_.view();
    }
    [[nodiscard]] usize render_count() const
    {
        return render_count_;
    }
    [[nodiscard]] sched::ProcessId process_id() const
    {
        return process_id_;
    }
    [[nodiscard]] const user::Credentials &credentials() const
    {
        return credentials_;
    }

  private:
    [[nodiscard]] Status require_directory_access(fs::VirtualFileSystem &vfs, std::string_view path) const;
    Status render(GuiCompositor &compositor, fs::VirtualFileSystem &vfs);

    SurfaceId surface_id_{0};
    FixedString<96> path_{"/"};
    usize render_count_{0};
    usize selected_entry_{fs::max_child_nodes};
    sched::ProcessId process_id_{0};
    user::Credentials credentials_{user::kernel_credentials()};
};

class GuiModule final : public KernelModule, public KernelService
{
  public:
    GuiModule() = default;
    explicit GuiModule(driver::FramebufferDisplayDriver &display)
    {
        bind_display(display);
    }

    void bind_display(driver::FramebufferDisplayDriver &display)
    {
        display_ = &display;
    }

    [[nodiscard]] ModuleManifest manifest() const override;
    [[nodiscard]] std::string_view service_id() const override
    {
        return gui_service_id;
    }
    Status start(ServiceRegistry &) override;
    Status stop() override;
    Status shutdown() override;

    [[nodiscard]] GuiCompositor &compositor()
    {
        return compositor_;
    }
    [[nodiscard]] const GuiCompositor &compositor() const
    {
        return compositor_;
    }

  private:
    driver::FramebufferDisplayDriver *display_{nullptr};
    GuiCompositor compositor_{};
};

class GuiSupervisor final
{
  public:
    GuiSupervisor(ModuleManager &modules, GuiModule &module) : modules_(modules), module_(module)
    {
    }

    Status tick();
    [[nodiscard]] usize restart_attempts() const
    {
        return restart_attempts_;
    }

  private:
    ModuleManager &modules_;
    GuiModule &module_;
    usize restart_attempts_{0};
};

[[nodiscard]] std::string_view gui_state_name(GuiState state);

} // namespace ok::gui
