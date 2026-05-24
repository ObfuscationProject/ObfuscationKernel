#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/module.hpp"
#include "ok/driver/driver.hpp"
#include "ok/driver/font.hpp"
#include "ok/fs/vfs.hpp"

#include <array>
#include <string_view>

namespace ok::gui
{

inline constexpr std::string_view gui_module_name{"kernel-gui"};
inline constexpr std::string_view gui_service_id{"gui.compositor"};
inline constexpr usize max_gui_surfaces = 4;
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

struct SurfaceInfo
{
    SurfaceId id{0};
    Rect bounds{};
    u32 z_index{0};
    bool visible{false};
    std::string_view title{};
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

  private:
    struct Surface
    {
        SurfaceId id{0};
        FixedString<max_gui_title> title{};
        Rect bounds{};
        u32 z_index{0};
        bool visible{true};
        std::array<u32, max_gui_surface_pixels> pixels{};
    };

    [[nodiscard]] Status ensure_running() const;
    [[nodiscard]] Result<usize> find_surface_index(SurfaceId id) const;
    [[nodiscard]] Status validate_bounds(Rect bounds) const;
    void draw_cell(Surface &surface, u32 column, u32 row, char value, u32 foreground, u32 background);
    Status draw_surface(const Surface &surface);
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
};

class KernelFileManager final
{
  public:
    Status open(GuiCompositor &compositor, fs::VirtualFileSystem &vfs, std::string_view path);
    Status refresh(GuiCompositor &compositor, fs::VirtualFileSystem &vfs);
    Status close(GuiCompositor &compositor);

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

  private:
    Status render(GuiCompositor &compositor, fs::VirtualFileSystem &vfs);

    SurfaceId surface_id_{0};
    FixedString<96> path_{"/"};
    usize render_count_{0};
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
