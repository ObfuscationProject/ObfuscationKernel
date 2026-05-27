#pragma once

#include "ok/gui/compositor.hpp"

namespace ok::gui
{

inline constexpr std::string_view gui_desktop_service_id{"gui.desktop"};
inline constexpr usize max_gui_shared_buffers = 8;
inline constexpr usize max_gui_shared_buffer_bytes = 4096;
inline constexpr usize max_gui_clipboard_text = 256;

using SharedBufferId = u16;

enum class DesktopBackend : u8
{
    kernel_compositor,
    user_service,
};

enum class GuiInputEventKind : u8
{
    key,
    pointer_delta,
    pointer_position,
    scroll,
};

struct GuiInputEvent
{
    GuiInputEventKind kind{GuiInputEventKind::key};
    int key{0};
    i32 x{0};
    i32 y{0};
    i32 delta_x{0};
    i32 delta_y{0};
    i32 scroll_rows{0};
    bool left_button{false};
};

struct GuiScanout
{
    u32 width{0};
    u32 height{0};
    u32 pitch{0};
    u8 bytes_per_pixel{4};
    bool active{false};
};

struct GuiCursorState
{
    i32 x{0};
    i32 y{0};
    bool visible{true};
};

struct GuiSharedBuffer
{
    SharedBufferId id{0};
    usize size{0};
    bool mapped{false};
    std::array<std::byte, max_gui_shared_buffer_bytes> bytes{};
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
    Status route_input(GuiInputEvent event);
    Status configure_scanout(GuiScanout scanout);
    Result<SharedBufferId> allocate_shared_buffer(usize size);
    Status release_shared_buffer(SharedBufferId id);
    Result<GuiSharedBuffer *> shared_buffer(SharedBufferId id);
    Result<const GuiSharedBuffer *> shared_buffer(SharedBufferId id) const;
    Status set_cursor(GuiCursorState cursor);
    Status write_clipboard(std::string_view text);
    [[nodiscard]] std::string_view clipboard_text() const
    {
        return clipboard_.view();
    }
    [[nodiscard]] GuiScanout scanout() const
    {
        return scanout_;
    }
    [[nodiscard]] GuiCursorState cursor() const
    {
        return cursor_;
    }
    [[nodiscard]] SurfaceId active_window() const;
    [[nodiscard]] usize window_count() const;
    [[nodiscard]] usize routed_key_count() const
    {
        return routed_key_count_;
    }
    [[nodiscard]] usize routed_input_count() const
    {
        return routed_input_count_;
    }
    [[nodiscard]] usize shared_buffer_count() const
    {
        return shared_buffer_count_;
    }

  private:
    [[nodiscard]] Result<GuiCompositor *> compositor();
    [[nodiscard]] Result<const GuiCompositor *> compositor() const;

    GuiCompositor *compositor_{nullptr};
    DesktopBackend backend_{DesktopBackend::kernel_compositor};
    usize routed_key_count_{0};
    usize routed_input_count_{0};
    GuiScanout scanout_{};
    GuiCursorState cursor_{};
    FixedString<max_gui_clipboard_text> clipboard_{};
    std::array<GuiSharedBuffer, max_gui_shared_buffers> shared_buffers_{};
    SharedBufferId next_shared_buffer_id_{1};
    usize shared_buffer_count_{0};
};

} // namespace ok::gui
