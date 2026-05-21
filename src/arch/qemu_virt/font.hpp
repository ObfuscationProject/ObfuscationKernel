#pragma once

#include "ok/core/types.hpp"

namespace ok::platform::qemu_virt
{

class BitmapFontRenderer final
{
  public:
    template <typename GlyphRow, typename PutPixel>
    static void draw_cell(char value,
                          u32 origin_x,
                          u32 origin_y,
                          u32 cell_width,
                          u32 cell_height,
                          u32 foreground,
                          u32 background,
                          GlyphRow glyph_row,
                          PutPixel put_pixel)
    {
        for (u32 y = 0; y < cell_height; ++y)
        {
            const u32 glyph_y = (y * glyph_height) / cell_height;
            const u8 row_bits = glyph_row(value, glyph_y);
            for (u32 x = 0; x < cell_width; ++x)
            {
                const u32 glyph_x = (x * glyph_width) / cell_width;
                const bool bit = ((row_bits >> (glyph_width - 1 - glyph_x)) & 1u) != 0;
                put_pixel(origin_x + x, origin_y + y, bit ? foreground : background);
            }
        }
    }

  private:
    static constexpr u32 glyph_width = 5;
    static constexpr u32 glyph_height = 7;
};

} // namespace ok::platform::qemu_virt
