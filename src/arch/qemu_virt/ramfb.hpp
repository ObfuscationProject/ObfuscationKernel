#pragma once

#include "ok/core/types.hpp"
#include "font.hpp"

namespace ok::platform::qemu_virt
{

template <uptr FwCfgBase, bool IoPort = false> class RamFbConsole
{
  public:
    static bool available()
    {
        initialize();
        return ready_;
    }

    static void clear()
    {
        initialize();
        if (!ready_)
        {
            return;
        }
        cursor_column_ = 0;
        cursor_row_ = 0;
        for (auto &pixel : pixels_)
        {
            pixel = background_color;
        }
    }

    static void write_char(char value)
    {
        initialize();
        if (!ready_)
        {
            return;
        }
        if (value == '\r')
        {
            return;
        }
        if (value == '\n')
        {
            newline();
            return;
        }
        if (value == '\f')
        {
            clear();
            return;
        }
        if (value == '\b')
        {
            erase_previous();
            return;
        }
        if (cursor_column_ >= text_columns)
        {
            newline();
        }
        draw_cell(cursor_column_, cursor_row_, value, foreground_color, background_color);
        ++cursor_column_;
    }

    static void draw_debug_marker()
    {
        initialize();
        if (!ready_)
        {
            return;
        }
        constexpr u32 marker_width = 72;
        constexpr u32 marker_height = 24;
        const u32 left = framebuffer_width - marker_width - 12;
        const u32 top = framebuffer_height - marker_height - 12;
        for (u32 y = 0; y < marker_height; ++y)
        {
            for (u32 x = 0; x < marker_width; ++x)
            {
                const bool border = x < 2 || y < 2 || x + 2 >= marker_width || y + 2 >= marker_height;
                const bool diagonal = ((x + y) % 11) < 5;
                const u32 color = border ? 0xfff4d35eu : (diagonal ? 0xff2dd4bcu : 0xff37a2ffu);
                put_pixel(left + x, top + y, color);
            }
        }
    }

    static void move_pointer(i32 delta_x, i32 delta_y, bool left_button)
    {
        initialize();
        if (!ready_)
        {
            return;
        }
        restore_pointer();
        pointer_x_ = clamp_pointer(static_cast<i32>(pointer_x_) + delta_x, framebuffer_width - pointer_width);
        pointer_y_ = clamp_pointer(static_cast<i32>(pointer_y_) + delta_y, framebuffer_height - pointer_height);
        draw_pointer(left_button);
    }

  private:
    struct DmaAccess
    {
        volatile u32 control;
        volatile u32 length;
        volatile u64 address;
    };

    struct RamFbConfig
    {
        u64 address;
        u32 fourcc;
        u32 flags;
        u32 width;
        u32 height;
        u32 stride;
    };

    static constexpr u32 framebuffer_width = 640;
    static constexpr u32 framebuffer_height = 400;
    static constexpr u32 framebuffer_stride = framebuffer_width * sizeof(u32);
    static constexpr usize framebuffer_pixels = static_cast<usize>(framebuffer_width) * framebuffer_height;
    static constexpr u32 cell_width = 8;
    static constexpr u32 cell_height = 16;
    static constexpr u32 text_columns = framebuffer_width / cell_width;
    static constexpr u32 text_rows = framebuffer_height / cell_height;
    static constexpr u32 background_color = 0xff061018u;
    static constexpr u32 foreground_color = 0xffd8f3ffu;
    static constexpr u32 pointer_width = 11;
    static constexpr u32 pointer_height = 15;
    static constexpr usize pointer_pixels = static_cast<usize>(pointer_width) * pointer_height;
    static constexpr u16 fw_cfg_signature = 0x0000;
    static constexpr u16 fw_cfg_file_dir = 0x0019;
    static constexpr u32 fw_cfg_dma_error = 1u << 0;
    static constexpr u32 fw_cfg_dma_select = 1u << 3;
    static constexpr u32 fw_cfg_dma_write = 1u << 4;
    static constexpr u32 ramfb_config_size = 28;

    static constexpr u16 bswap16(u16 value)
    {
        return static_cast<u16>((value << 8) | (value >> 8));
    }

    static constexpr u32 bswap32(u32 value)
    {
        return ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
               ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
    }

    static constexpr u64 bswap64(u64 value)
    {
        return (static_cast<u64>(bswap32(static_cast<u32>(value & 0xffff'ffffull))) << 32) |
               bswap32(static_cast<u32>((value >> 32) & 0xffff'ffffull));
    }

    static constexpr u32 fourcc(char a, char b, char c, char d)
    {
        return static_cast<u32>(a) | (static_cast<u32>(b) << 8) | (static_cast<u32>(c) << 16) |
               (static_cast<u32>(d) << 24);
    }

    static volatile u8 &mmio8(uptr address)
    {
        return *reinterpret_cast<volatile u8 *>(address);
    }

    static volatile u16 &mmio16(uptr address)
    {
        return *reinterpret_cast<volatile u16 *>(address);
    }

    static volatile u64 &mmio64(uptr address)
    {
        return *reinterpret_cast<volatile u64 *>(address);
    }

    static void initialize()
    {
        if (initialized_)
        {
            return;
        }
        initialized_ = true;
        for (auto &pixel : pixels_)
        {
            pixel = background_color;
        }
        u16 ramfb_selector = 0;
        if (!fw_cfg_present() || !find_file("etc/ramfb", ramfb_selector))
        {
            return;
        }
        ramfb_config_ = RamFbConfig{
            .address = bswap64(reinterpret_cast<uptr>(pixels_)),
            .fourcc = bswap32(fourcc('X', 'R', '2', '4')),
            .flags = 0,
            .width = bswap32(framebuffer_width),
            .height = bswap32(framebuffer_height),
            .stride = bswap32(framebuffer_stride),
        };
        ready_ = dma_write(ramfb_selector, &ramfb_config_, ramfb_config_size);
    }

    static bool fw_cfg_present()
    {
        fw_cfg_select(fw_cfg_signature);
        return fw_cfg_read8() == 'Q' && fw_cfg_read8() == 'E' && fw_cfg_read8() == 'M' && fw_cfg_read8() == 'U';
    }

    static u8 fw_cfg_read8()
    {
        if constexpr (IoPort)
        {
            return port_inb(static_cast<u16>(FwCfgBase + 1));
        }
        else
        {
            return mmio8(FwCfgBase);
        }
    }

    static u16 fw_cfg_read_be16()
    {
        u16 value = 0;
        value = static_cast<u16>((value << 8) | fw_cfg_read8());
        value = static_cast<u16>((value << 8) | fw_cfg_read8());
        return value;
    }

    static u32 fw_cfg_read_be32()
    {
        u32 value = 0;
        for (usize i = 0; i < 4; ++i)
        {
            value = (value << 8) | fw_cfg_read8();
        }
        return value;
    }

    static void fw_cfg_skip(usize count)
    {
        for (usize i = 0; i < count; ++i)
        {
            static_cast<void>(fw_cfg_read8());
        }
    }

    static bool find_file(const char *target, u16 &selector)
    {
        fw_cfg_select(fw_cfg_file_dir);
        const u32 count = fw_cfg_read_be32();
        for (u32 entry = 0; entry < count && entry < 256; ++entry)
        {
            static_cast<void>(fw_cfg_read_be32());
            const u16 file_selector = fw_cfg_read_be16();
            fw_cfg_skip(2);

            bool matches = true;
            bool target_end_seen = false;
            for (usize i = 0; i < 56; ++i)
            {
                const char actual = static_cast<char>(fw_cfg_read8());
                if (target_end_seen)
                {
                    continue;
                }
                const char expected = target[i];
                if (actual != expected)
                {
                    matches = false;
                }
                if (expected == '\0')
                {
                    target_end_seen = true;
                }
            }
            if (matches && target_end_seen)
            {
                selector = file_selector;
                return true;
            }
        }
        return false;
    }

    static bool dma_write(u16 selector, const void *data, u32 length)
    {
        dma_access_.control = bswap32((static_cast<u32>(selector) << 16) | fw_cfg_dma_select | fw_cfg_dma_write);
        dma_access_.length = bswap32(length);
        dma_access_.address = bswap64(reinterpret_cast<uptr>(data));
        asm volatile("" ::: "memory");
        write_dma_address(reinterpret_cast<uptr>(&dma_access_));
        for (usize attempt = 0; attempt < 1000000; ++attempt)
        {
            asm volatile("" ::: "memory");
            const u32 control = bswap32(dma_access_.control);
            if (control == 0)
            {
                return true;
            }
            if ((control & fw_cfg_dma_error) != 0)
            {
                return false;
            }
        }
        return false;
    }

    static void put_pixel(u32 x, u32 y, u32 color)
    {
        if (x < framebuffer_width && y < framebuffer_height)
        {
            pixels_[static_cast<usize>(y) * framebuffer_width + x] = color;
        }
    }

    static u32 clamp_pointer(i32 value, u32 maximum)
    {
        if (value < 0)
        {
            return 0;
        }
        const auto converted = static_cast<u32>(value);
        return converted > maximum ? maximum : converted;
    }

    static void restore_pointer()
    {
        if (!pointer_drawn_)
        {
            return;
        }
        for (u32 y = 0; y < pointer_height; ++y)
        {
            for (u32 x = 0; x < pointer_width; ++x)
            {
                put_pixel(pointer_x_ + x, pointer_y_ + y,
                          pointer_saved_[static_cast<usize>(y) * pointer_width + x]);
            }
        }
        pointer_drawn_ = false;
    }

    static void draw_pointer(bool left_button)
    {
        constexpr u16 shape[] = {
            0b10000000000,
            0b11000000000,
            0b11100000000,
            0b11110000000,
            0b11111000000,
            0b11111100000,
            0b11111110000,
            0b11111111000,
            0b11111111100,
            0b11111000000,
            0b11011000000,
            0b10011000000,
            0b00001100000,
            0b00001100000,
            0b00000110000,
        };
        const u32 fill = left_button ? 0xfff4d35eu : 0xffffffffu;
        for (u32 y = 0; y < pointer_height; ++y)
        {
            for (u32 x = 0; x < pointer_width; ++x)
            {
                const auto index = static_cast<usize>(y) * pointer_width + x;
                pointer_saved_[index] = pixels_[static_cast<usize>(pointer_y_ + y) * framebuffer_width + pointer_x_ + x];
                if (((shape[y] >> (pointer_width - 1 - x)) & 1u) != 0)
                {
                    const bool edge = x == 0 || y == 0 || ((shape[y] >> (pointer_width - x)) & 1u) == 0 ||
                                      y + 1 == pointer_height || ((shape[y + 1] >> (pointer_width - 1 - x)) & 1u) == 0;
                    put_pixel(pointer_x_ + x, pointer_y_ + y, edge ? 0xff000000u : fill);
                }
            }
        }
        pointer_drawn_ = true;
    }

    static u8 glyph_row(char value, u32 row)
    {
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

    static void draw_cell(u32 column, u32 row, char value, u32 foreground, u32 background)
    {
        const u32 origin_x = column * cell_width;
        const u32 origin_y = row * cell_height;
        BitmapFontRenderer::draw_cell(
            value, origin_x, origin_y, cell_width, cell_height, foreground, background,
            [](char ch, u32 glyph_y) { return glyph_row(ch, glyph_y); },
            [](u32 x, u32 y, u32 color) { put_pixel(x, y, color); });
    }

    static void write_dma_address(uptr address)
    {
        if constexpr (IoPort)
        {
            const auto wide_address = static_cast<u64>(address);
            port_outl(static_cast<u16>(FwCfgBase + 4),
                      bswap32(static_cast<u32>((wide_address >> 32) & 0xffff'ffffull)));
            port_outl(static_cast<u16>(FwCfgBase + 8), bswap32(static_cast<u32>(wide_address & 0xffff'ffffull)));
        }
        else
        {
            mmio64(FwCfgBase + 16) = bswap64(address);
        }
    }

    static void fw_cfg_select(u16 selector)
    {
        if constexpr (IoPort)
        {
            port_outw(static_cast<u16>(FwCfgBase), selector);
        }
        else
        {
            mmio16(FwCfgBase + 8) = bswap16(selector);
        }
    }

    static u8 port_inb(u16 port)
    {
        u8 value = 0;
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }

    static void port_outw(u16 port, u16 value)
    {
        asm volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
    }

    static void port_outl(u16 port, u32 value)
    {
        asm volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
    }

    static void erase_cell(u32 column, u32 row)
    {
        draw_cell(column, row, ' ', foreground_color, background_color);
    }

    static void erase_previous()
    {
        if (cursor_column_ == 0)
        {
            return;
        }
        --cursor_column_;
        erase_cell(cursor_column_, cursor_row_);
    }

    static void newline()
    {
        cursor_column_ = 0;
        ++cursor_row_;
        if (cursor_row_ >= text_rows)
        {
            cursor_row_ = 0;
            clear();
        }
    }

    static inline bool initialized_{false};
    static inline bool ready_{false};
    static inline u32 cursor_column_{0};
    static inline u32 cursor_row_{0};
    static inline u32 pointer_x_{framebuffer_width / 2};
    static inline u32 pointer_y_{framebuffer_height / 2};
    static inline bool pointer_drawn_{false};
    static inline u32 pointer_saved_[pointer_pixels]{};
    alignas(4096) static inline u32 pixels_[framebuffer_pixels]{};
    alignas(16) static inline volatile DmaAccess dma_access_{};
    alignas(16) static inline RamFbConfig ramfb_config_{};
};

} // namespace ok::platform::qemu_virt
