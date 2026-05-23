#pragma once

#include "ok/core/types.hpp"

namespace ok::driver
{

class BitmapFontRenderer final
{
  public:
    static constexpr u32 glyph_width = 5;
    static constexpr u32 glyph_height = 7;

    [[nodiscard]] static u8 glyph_row(char value, u32 row)
    {
        if (row >= glyph_height)
        {
            return 0;
        }

        switch (value)
        {
        case 'A': {
            constexpr u8 rows[] = {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
            return rows[row];
        }
        case 'B': {
            constexpr u8 rows[] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110};
            return rows[row];
        }
        case 'C': {
            constexpr u8 rows[] = {0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111};
            return rows[row];
        }
        case 'D': {
            constexpr u8 rows[] = {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110};
            return rows[row];
        }
        case 'E': {
            constexpr u8 rows[] = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111};
            return rows[row];
        }
        case 'F': {
            constexpr u8 rows[] = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000};
            return rows[row];
        }
        case 'G': {
            constexpr u8 rows[] = {0b01111, 0b10000, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111};
            return rows[row];
        }
        case 'H': {
            constexpr u8 rows[] = {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
            return rows[row];
        }
        case 'I': {
            constexpr u8 rows[] = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111};
            return rows[row];
        }
        case 'J': {
            constexpr u8 rows[] = {0b00111, 0b00010, 0b00010, 0b00010, 0b10010, 0b10010, 0b01100};
            return rows[row];
        }
        case 'K': {
            constexpr u8 rows[] = {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001};
            return rows[row];
        }
        case 'L': {
            constexpr u8 rows[] = {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111};
            return rows[row];
        }
        case 'M': {
            constexpr u8 rows[] = {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001};
            return rows[row];
        }
        case 'N': {
            constexpr u8 rows[] = {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001};
            return rows[row];
        }
        case 'O': {
            constexpr u8 rows[] = {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
            return rows[row];
        }
        case 'P': {
            constexpr u8 rows[] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000};
            return rows[row];
        }
        case 'Q': {
            constexpr u8 rows[] = {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101};
            return rows[row];
        }
        case 'R': {
            constexpr u8 rows[] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001};
            return rows[row];
        }
        case 'S': {
            constexpr u8 rows[] = {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110};
            return rows[row];
        }
        case 'T': {
            constexpr u8 rows[] = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
            return rows[row];
        }
        case 'U': {
            constexpr u8 rows[] = {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
            return rows[row];
        }
        case 'V': {
            constexpr u8 rows[] = {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100};
            return rows[row];
        }
        case 'W': {
            constexpr u8 rows[] = {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010};
            return rows[row];
        }
        case 'X': {
            constexpr u8 rows[] = {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001};
            return rows[row];
        }
        case 'Y': {
            constexpr u8 rows[] = {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100};
            return rows[row];
        }
        case 'Z': {
            constexpr u8 rows[] = {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111};
            return rows[row];
        }
        case 'a': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b01110, 0b00001, 0b01111, 0b10001, 0b01111};
            return rows[row];
        }
        case 'b': {
            constexpr u8 rows[] = {0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b11110};
            return rows[row];
        }
        case 'c': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b01111, 0b10000, 0b10000, 0b10000, 0b01111};
            return rows[row];
        }
        case 'd': {
            constexpr u8 rows[] = {0b00001, 0b00001, 0b01101, 0b10011, 0b10001, 0b10001, 0b01111};
            return rows[row];
        }
        case 'e': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01111};
            return rows[row];
        }
        case 'f': {
            constexpr u8 rows[] = {0b00110, 0b01000, 0b01000, 0b11110, 0b01000, 0b01000, 0b01000};
            return rows[row];
        }
        case 'g': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b01111, 0b10001, 0b01111, 0b00001, 0b01110};
            return rows[row];
        }
        case 'h': {
            constexpr u8 rows[] = {0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001};
            return rows[row];
        }
        case 'i': {
            constexpr u8 rows[] = {0b00100, 0b00000, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110};
            return rows[row];
        }
        case 'j': {
            constexpr u8 rows[] = {0b00010, 0b00000, 0b00110, 0b00010, 0b00010, 0b10010, 0b01100};
            return rows[row];
        }
        case 'k': {
            constexpr u8 rows[] = {0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010};
            return rows[row];
        }
        case 'l': {
            constexpr u8 rows[] = {0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110};
            return rows[row];
        }
        case 'm': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b11010, 0b10101, 0b10101, 0b10101, 0b10101};
            return rows[row];
        }
        case 'n': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001};
            return rows[row];
        }
        case 'o': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110};
            return rows[row];
        }
        case 'p': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b11110, 0b10001, 0b11110, 0b10000, 0b10000};
            return rows[row];
        }
        case 'q': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b01111, 0b10001, 0b01111, 0b00001, 0b00001};
            return rows[row];
        }
        case 'r': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b10110, 0b11001, 0b10000, 0b10000, 0b10000};
            return rows[row];
        }
        case 's': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b01111, 0b10000, 0b01110, 0b00001, 0b11110};
            return rows[row];
        }
        case 't': {
            constexpr u8 rows[] = {0b01000, 0b01000, 0b11110, 0b01000, 0b01000, 0b01001, 0b00110};
            return rows[row];
        }
        case 'u': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b10011, 0b01101};
            return rows[row];
        }
        case 'v': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100};
            return rows[row];
        }
        case 'w': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010};
            return rows[row];
        }
        case 'x': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001};
            return rows[row];
        }
        case 'y': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110};
            return rows[row];
        }
        case 'z': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b11111, 0b00010, 0b00100, 0b01000, 0b11111};
            return rows[row];
        }
        case '0': {
            constexpr u8 rows[] = {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110};
            return rows[row];
        }
        case '1': {
            constexpr u8 rows[] = {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110};
            return rows[row];
        }
        case '2': {
            constexpr u8 rows[] = {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111};
            return rows[row];
        }
        case '3': {
            constexpr u8 rows[] = {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110};
            return rows[row];
        }
        case '4': {
            constexpr u8 rows[] = {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010};
            return rows[row];
        }
        case '5': {
            constexpr u8 rows[] = {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110};
            return rows[row];
        }
        case '6': {
            constexpr u8 rows[] = {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110};
            return rows[row];
        }
        case '7': {
            constexpr u8 rows[] = {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000};
            return rows[row];
        }
        case '8': {
            constexpr u8 rows[] = {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110};
            return rows[row];
        }
        case '9': {
            constexpr u8 rows[] = {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100};
            return rows[row];
        }
        case '[': {
            constexpr u8 rows[] = {0b11100, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11100};
            return rows[row];
        }
        case ']': {
            constexpr u8 rows[] = {0b00111, 0b00001, 0b00001, 0b00001, 0b00001, 0b00001, 0b00111};
            return rows[row];
        }
        case '(':
        case '<': {
            constexpr u8 rows[] = {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010};
            return rows[row];
        }
        case ')':
        case '>': {
            constexpr u8 rows[] = {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000};
            return rows[row];
        }
        case '-': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000};
            return rows[row];
        }
        case '_': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111};
            return rows[row];
        }
        case '=': {
            constexpr u8 rows[] = {0b00000, 0b11111, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000};
            return rows[row];
        }
        case '+': {
            constexpr u8 rows[] = {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000};
            return rows[row];
        }
        case '.': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100};
            return rows[row];
        }
        case ':': {
            constexpr u8 rows[] = {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000};
            return rows[row];
        }
        case '/': {
            constexpr u8 rows[] = {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000};
            return rows[row];
        }
        case '\\': {
            constexpr u8 rows[] = {0b10000, 0b01000, 0b01000, 0b00100, 0b00010, 0b00010, 0b00001};
            return rows[row];
        }
        case '|': {
            constexpr u8 rows[] = {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
            return rows[row];
        }
        case ',': {
            constexpr u8 rows[] = {0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b00100, 0b01000};
            return rows[row];
        }
        case '\'':
        case '`': {
            constexpr u8 rows[] = {0b00100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000};
            return rows[row];
        }
        case '"': {
            constexpr u8 rows[] = {0b01010, 0b01010, 0b01010, 0b00000, 0b00000, 0b00000, 0b00000};
            return rows[row];
        }
        case ' ':
            return 0;
        default: {
            constexpr u8 rows[] = {0b11111, 0b10001, 0b00101, 0b00110, 0b00100, 0b00000, 0b00100};
            return rows[row];
        }
        }
    }

    template <typename GlyphRow, typename PutPixel>
    static void draw_cell(char value,
                          u32 origin_x,
                          u32 origin_y,
                          u32 cell_width,
                          u32 cell_height,
                          u32 foreground,
                          u32 background,
                          GlyphRow row_bits,
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
                const u8 bits = row_bits(value, glyph_y);
                const bool bit = ((bits >> (glyph_width - 1 - glyph_x)) & 1u) != 0;
                put_pixel(origin_x + x, origin_y + y, bit ? foreground : background);
            }
        }
    }
};

} // namespace ok::driver
