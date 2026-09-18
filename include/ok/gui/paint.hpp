#pragma once
#include "ok/gui/compositor.hpp"
#include "ok/gui/theme.hpp"

namespace ok::gui
{
// Bounded immediate drawing, with one accumulated error and no virtual pixel dispatch.
class Painter final
{
  public:
    Painter(GuiCompositor &compositor, SurfaceId surface) : compositor_(compositor), surface_(surface)
    {
    }
    void rect(i32 x, i32 y, u32 width, u32 height, u32 color)
    {
        if (status_.ok())
            status_ = compositor_.fill_rect(surface_, Rect{x, y, width, height}, color);
    }
    void text(i32 x, i32 y, std::string_view value, u32 color = theme::text, u32 background = theme::surface,
              u32 width = max_gui_surface_width, u32 scale = 1)
    {
        if (status_.ok())
            status_ = compositor_.draw_text_at(surface_, x, y, value, color, background, width, scale);
    }
    void outline(Rect bounds, u32 color = theme::border)
    {
        if (bounds.width == 0 || bounds.height == 0)
            return;
        rect(bounds.x, bounds.y, bounds.width, 1, color);
        rect(bounds.x, bounds.y + static_cast<i32>(bounds.height) - 1, bounds.width, 1, color);
        rect(bounds.x, bounds.y, 1, bounds.height, color);
        rect(bounds.x + static_cast<i32>(bounds.width) - 1, bounds.y, 1, bounds.height, color);
    }
    void rounded(Rect bounds, u32 color, u32 radius = 4)
    {
        if (bounds.width < radius * 2 || bounds.height < radius * 2)
        {
            rect(bounds.x, bounds.y, bounds.width, bounds.height, color);
            return;
        }
        for (u32 row = 0; row < bounds.height; ++row)
        {
            const auto edge =
                row < radius ? radius - row : (row >= bounds.height - radius ? row - (bounds.height - radius) + 1 : 0);
            const auto inset = edge > 2 ? 2u : (edge > 0 ? 1u : 0u);
            rect(bounds.x + static_cast<i32>(inset), bounds.y + static_cast<i32>(row), bounds.width - inset * 2, 1,
                 color);
        }
    }
    [[nodiscard]] Status status() const
    {
        return status_;
    }

  private:
    GuiCompositor &compositor_;
    SurfaceId surface_;
    Status status_{};
};
} // namespace ok::gui
