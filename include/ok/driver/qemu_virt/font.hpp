#pragma once

#include "ok/core/types.hpp"

namespace ok::driver::qemu_virt
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
        const u32 horizontal_padding = cell_width > 4 ? 1 : 0;
        const u32 vertical_padding = cell_height > 8 ? 2 : 0;
        const u32 drawable_width = cell_width - horizontal_padding * 2;
        const u32 drawable_height = cell_height - vertical_padding * 2;
        for (u32 y = 0; y < cell_height; ++y)
        {
            for (u32 x = 0; x < cell_width; ++x)
            {
                if (x < horizontal_padding || x >= horizontal_padding + drawable_width || y < vertical_padding ||
                    y >= vertical_padding + drawable_height)
                {
                    put_pixel(origin_x + x, origin_y + y, background);
                    continue;
                }
                const u32 glyph_x = ((x - horizontal_padding) * glyph_width) / drawable_width;
                const u32 glyph_y = ((y - vertical_padding) * glyph_height) / drawable_height;
                const u8 row_bits = glyph_row(value, glyph_y);
                const bool bit = ((row_bits >> (glyph_width - 1 - glyph_x)) & 1u) != 0;
                put_pixel(origin_x + x, origin_y + y, bit ? foreground : background);
            }
        }
    }

  private:
    static constexpr u32 glyph_width = 5;
    static constexpr u32 glyph_height = 7;
};

} // namespace ok::driver::qemu_virt
