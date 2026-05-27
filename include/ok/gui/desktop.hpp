#pragma once

#include "ok/gui/compositor.hpp"

namespace ok::gui
{

inline constexpr std::string_view gui_desktop_service_id{"gui.desktop"};

enum class DesktopBackend : u8
{
    kernel_compositor,
    user_service,
};

struct DesktopWindowRequest
{
    Rect bounds{};
    std::string_view title{};
    TaskbarApp app{TaskbarApp::none};
    bool visible{true};
};

class GuiDesktopService final
{
  public:
    [[nodiscard]] std::string_view service_id() const
    {
        return gui_desktop_service_id;
    }
    Status bind(GuiCompositor &compositor, DesktopBackend backend = DesktopBackend::kernel_compositor);
    Status unbind();
    [[nodiscard]] bool bound() const
    {
        return compositor_ != nullptr;
    }
    [[nodiscard]] DesktopBackend backend() const
    {
        return backend_;
    }
    Result<SurfaceId> open_window(DesktopWindowRequest request);
    Status close_window(SurfaceId id);
    Status focus_window(SurfaceId id);
    Status route_pointer_delta(i32 delta_x, i32 delta_y, bool left_button);
    Status route_key(int key);
    [[nodiscard]] SurfaceId active_window() const;
    [[nodiscard]] usize window_count() const;
    [[nodiscard]] usize routed_key_count() const
    {
        return routed_key_count_;
    }

  private:
    [[nodiscard]] Result<GuiCompositor *> compositor();
    [[nodiscard]] Result<const GuiCompositor *> compositor() const;

    GuiCompositor *compositor_{nullptr};
    DesktopBackend backend_{DesktopBackend::kernel_compositor};
    usize routed_key_count_{0};
};

} // namespace ok::gui
