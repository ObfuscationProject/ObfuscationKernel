#include "ok/gui/desktop.hpp"

namespace ok::gui
{

Status GuiDesktopService::bind(GuiCompositor &compositor, DesktopBackend backend)
{
    compositor_ = &compositor;
    backend_ = backend;
    routed_key_count_ = 0;
    routed_input_count_ = 0;
    const auto bounds = compositor.desktop_bounds();
    if (bounds)
    {
        scanout_ = GuiScanout{
            .width = bounds.value().width,
            .height = bounds.value().height,
            .pitch = static_cast<u32>(bounds.value().width * sizeof(u32)),
            .bytes_per_pixel = sizeof(u32),
            .active = true,
        };
    }
    cursor_ = GuiCursorState{.x = compositor.pointer_x(), .y = compositor.pointer_y(), .visible = true};
    return Status::success();
}

Status GuiDesktopService::unbind()
{
    compositor_ = nullptr;
    routed_key_count_ = 0;
    routed_input_count_ = 0;
    scanout_ = {};
    cursor_ = {};
    clipboard_.clear();
    for (auto &buffer : shared_buffers_)
    {
        buffer = {};
    }
    shared_buffer_count_ = 0;
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
    ++routed_input_count_;
    return Status::success();
}

Status GuiDesktopService::route_input(GuiInputEvent event)
{
    auto compositor_result = compositor();
    if (!compositor_result)
    {
        return compositor_result.status();
    }
    switch (event.kind)
    {
    case GuiInputEventKind::key:
        return route_key(event.key);
    case GuiInputEventKind::pointer_delta:
        ++routed_input_count_;
        return route_pointer_delta(event.delta_x, event.delta_y, event.left_button);
    case GuiInputEventKind::pointer_position:
        ++routed_input_count_;
        if (auto status = compositor_result.value()->set_pointer_position(event.x, event.y); !status.ok())
        {
            return status;
        }
        cursor_.x = compositor_result.value()->pointer_x();
        cursor_.y = compositor_result.value()->pointer_y();
        return Status::success();
    case GuiInputEventKind::scroll:
        ++routed_input_count_;
        return Status::success();
    }
    return Status::success();
}

Status GuiDesktopService::configure_scanout(GuiScanout scanout)
{
    if (scanout.width == 0 || scanout.height == 0 || scanout.pitch == 0 || scanout.bytes_per_pixel == 0)
    {
        return Status::invalid_argument("GUI scanout geometry is invalid");
    }
    scanout.active = true;
    scanout_ = scanout;
    return Status::success();
}

Result<SharedBufferId> GuiDesktopService::allocate_shared_buffer(usize size)
{
    if (size == 0 || size > max_gui_shared_buffer_bytes)
    {
        return Status::invalid_argument("GUI shared buffer size is invalid");
    }
    for (auto &buffer : shared_buffers_)
    {
        if (buffer.mapped)
        {
            continue;
        }
        buffer = GuiSharedBuffer{.id = next_shared_buffer_id_++, .size = size, .mapped = true};
        ++shared_buffer_count_;
        return buffer.id;
    }
    return Status::overflow("GUI shared buffer table is full");
}

Status GuiDesktopService::release_shared_buffer(SharedBufferId id)
{
    for (auto &buffer : shared_buffers_)
    {
        if (!buffer.mapped || buffer.id != id)
        {
            continue;
        }
        buffer = {};
        if (shared_buffer_count_ > 0)
        {
            --shared_buffer_count_;
        }
        return Status::success();
    }
    return Status::not_found("GUI shared buffer was not found");
}

Result<GuiSharedBuffer *> GuiDesktopService::shared_buffer(SharedBufferId id)
{
    for (auto &buffer : shared_buffers_)
    {
        if (buffer.mapped && buffer.id == id)
        {
            return &buffer;
        }
    }
    return Status::not_found("GUI shared buffer was not found");
}

Result<const GuiSharedBuffer *> GuiDesktopService::shared_buffer(SharedBufferId id) const
{
    for (const auto &buffer : shared_buffers_)
    {
        if (buffer.mapped && buffer.id == id)
        {
            return &buffer;
        }
    }
    return Status::not_found("GUI shared buffer was not found");
}

Status GuiDesktopService::set_cursor(GuiCursorState cursor)
{
    cursor_ = cursor;
    if (compositor_ == nullptr)
    {
        return Status::success();
    }
    return compositor_->set_pointer_position(cursor.x, cursor.y);
}

Status GuiDesktopService::write_clipboard(std::string_view text)
{
    return clipboard_.assign(text);
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
