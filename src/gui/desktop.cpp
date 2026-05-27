#include "ok/gui/desktop.hpp"

namespace ok::gui
{

Status GuiDesktopService::bind(GuiCompositor &compositor, DesktopBackend backend)
{
    compositor_ = &compositor;
    backend_ = backend;
    routed_key_count_ = 0;
    return Status::success();
}

Status GuiDesktopService::unbind()
{
    compositor_ = nullptr;
    routed_key_count_ = 0;
    return Status::success();
}

Result<GuiCompositor *> GuiDesktopService::compositor()
{
    if (compositor_ == nullptr)
    {
        return Status::not_initialized("GUI desktop service is not bound");
    }
    return compositor_;
}

Result<const GuiCompositor *> GuiDesktopService::compositor() const
{
    if (compositor_ == nullptr)
    {
        return Status::not_initialized("GUI desktop service is not bound");
    }
    return compositor_;
}

Result<SurfaceId> GuiDesktopService::open_window(DesktopWindowRequest request)
{
    auto compositor_result = compositor();
    if (!compositor_result)
    {
        return compositor_result.status();
    }
    auto surface = compositor_result.value()->create_surface(request.bounds, request.title);
    if (!surface)
    {
        return surface.status();
    }
    if (auto status = compositor_result.value()->set_surface_app(surface.value(), request.app); !status.ok())
    {
        static_cast<void>(compositor_result.value()->destroy_surface(surface.value()));
        return status;
    }
    if (auto status = compositor_result.value()->set_visible(surface.value(), request.visible); !status.ok())
    {
        static_cast<void>(compositor_result.value()->destroy_surface(surface.value()));
        return status;
    }
    return surface.value();
}

Status GuiDesktopService::close_window(SurfaceId id)
{
    auto compositor_result = compositor();
    if (!compositor_result)
    {
        return compositor_result.status();
    }
    return compositor_result.value()->close_surface(id);
}

Status GuiDesktopService::focus_window(SurfaceId id)
{
    auto compositor_result = compositor();
    if (!compositor_result)
    {
        return compositor_result.status();
    }
    return compositor_result.value()->raise_surface(id);
}

Status GuiDesktopService::route_pointer_delta(i32 delta_x, i32 delta_y, bool left_button)
{
    auto compositor_result = compositor();
    if (!compositor_result)
    {
        return compositor_result.status();
    }
    return compositor_result.value()->handle_mouse_delta(delta_x, delta_y, left_button);
}

Status GuiDesktopService::route_key(int)
{
    if (compositor_ == nullptr)
    {
        return Status::not_initialized("GUI desktop service is not bound");
    }
    ++routed_key_count_;
    return Status::success();
}

SurfaceId GuiDesktopService::active_window() const
{
    auto compositor_result = compositor();
    if (!compositor_result)
    {
        return 0;
    }
    return compositor_result.value()->active_surface();
}

usize GuiDesktopService::window_count() const
{
    auto compositor_result = compositor();
    if (!compositor_result)
    {
        return 0;
    }
    return compositor_result.value()->surface_count();
}

} // namespace ok::gui
