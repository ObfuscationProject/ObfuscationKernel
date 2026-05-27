#pragma once

#include "ok/core/types.hpp"
#include "ok/driver/driver.hpp"
#include "font.hpp"

namespace ok::driver::qemu_virt
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
        gui_active_ = false;
        pointer_drawn_ = false;
        pointer_x_ = framebuffer_width / 2;
        pointer_y_ = framebuffer_height / 2;
        pointer_left_button_ = false;
        gui_delta_x_remainder_ = 0;
        gui_delta_y_remainder_ = 0;
        for (auto &pixel : pixels_)
        {
            pixel = background_color;
        }
        draw_mouse_status();
        draw_pointer(false);
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
        const u32 top = framebuffer_height - cell_height - marker_height - 12;
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
        pointer_left_button_ = left_button;
        if (!gui_active_)
        {
            draw_mouse_status();
        }
    }

    static void redraw_pointer_after_gui_present()
    {
        initialize();
        if (!ready_)
        {
            return;
        }
        if (!pointer_drawn_)
        {
            draw_pointer(pointer_left_button_);
        }
    }

    static void begin_gui_frame()
    {
        initialize();
        if (!ready_)
        {
            return;
        }
        gui_active_ = true;
        restore_pointer();
    }

    static void draw_gui_frame(u32 logical_width, u32 logical_height, const u32 *frame, usize pixel_count)
    {
        initialize();
        if (!ready_ || logical_width == 0 || logical_height == 0 || frame == nullptr ||
            pixel_count < static_cast<usize>(logical_width) * logical_height)
        {
            return;
        }
        gui_active_ = true;
        for (u32 target_y = 0; target_y < framebuffer_height; ++target_y)
        {
            const auto source_y = static_cast<u32>((static_cast<u64>(target_y) * logical_height) / framebuffer_height);
            const auto *source_row = frame + static_cast<usize>(source_y) * logical_width;
            for (u32 target_x = 0; target_x < framebuffer_width; ++target_x)
            {
                const auto source_x =
                    static_cast<u32>((static_cast<u64>(target_x) * logical_width) / framebuffer_width);
                put_pixel(target_x, target_y, source_row[source_x]);
            }
        }
    }

    static void end_gui_frame()
    {
        initialize();
        if (!ready_)
        {
            return;
        }
        if (!pointer_drawn_)
        {
            draw_pointer(pointer_left_button_);
        }
    }

    static i32 gui_delta_x(i32 delta_x)
    {
        return scale_gui_delta(delta_x, static_cast<i32>(ok::driver::framebuffer_width),
                               static_cast<i32>(framebuffer_width), gui_delta_x_remainder_);
    }

    static i32 gui_delta_y(i32 delta_y)
    {
        return scale_gui_delta(delta_y, static_cast<i32>(ok::driver::framebuffer_height),
                               static_cast<i32>(framebuffer_height), gui_delta_y_remainder_);
    }

    static i32 gui_pointer_x()
    {
        initialize();
        return static_cast<i32>((static_cast<u64>(pointer_x_) * ok::driver::framebuffer_width) / framebuffer_width);
    }

    static i32 gui_pointer_y()
    {
        initialize();
        return static_cast<i32>((static_cast<u64>(pointer_y_) * ok::driver::framebuffer_height) / framebuffer_height);
    }

    static void draw_gui_pixel(u32 logical_width, u32 logical_height, u32 x, u32 y, u32 color)
    {
        initialize();
        if (!ready_ || logical_width == 0 || logical_height == 0 || x >= logical_width || y >= logical_height)
        {
            return;
        }
        gui_active_ = true;
        const u32 left = (x * framebuffer_width) / logical_width;
        u32 right = ((x + 1) * framebuffer_width) / logical_width;
        const u32 top = (y * framebuffer_height) / logical_height;
        u32 bottom = ((y + 1) * framebuffer_height) / logical_height;
        if (right <= left)
        {
            right = left + 1;
        }
        if (bottom <= top)
        {
            bottom = top + 1;
        }
        for (u32 target_y = top; target_y < bottom && target_y < framebuffer_height; ++target_y)
        {
            for (u32 target_x = left; target_x < right && target_x < framebuffer_width; ++target_x)
            {
                put_pixel(target_x, target_y, color);
            }
        }
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

    static constexpr u32 framebuffer_width = 960;
    static constexpr u32 framebuffer_height = 540;
    static constexpr u32 framebuffer_stride = framebuffer_width * sizeof(u32);
    static constexpr usize framebuffer_pixels = static_cast<usize>(framebuffer_width) * framebuffer_height;
    static constexpr u32 cell_width = 10;
    static constexpr u32 cell_height = 18;
    static constexpr u32 text_columns = framebuffer_width / cell_width;
    static constexpr u32 text_rows = framebuffer_height / cell_height - 1;
    static constexpr u32 mouse_status_row = text_rows;
    static constexpr u32 background_color = 0xff061018u;
    static constexpr u32 foreground_color = 0xffd8f3ffu;
    static constexpr u32 mouse_status_background = 0xff102030u;
    static constexpr u32 mouse_status_foreground = 0xffbfe7ffu;
    static constexpr u32 mouse_status_pressed_foreground = 0xfff4d35eu;
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

    static i32 scale_gui_delta(i32 delta, i32 logical_extent, i32 physical_extent, i32 &remainder)
    {
        if (physical_extent <= 0)
        {
            return delta;
        }
        const auto numerator = delta * logical_extent + remainder;
        const auto scaled = numerator / physical_extent;
        remainder = numerator - scaled * physical_extent;
        return scaled;
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
        if (ready_)
        {
            draw_mouse_status();
            draw_pointer(false);
        }
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

    static void draw_cell(u32 column, u32 row, char value, u32 foreground, u32 background)
    {
        const u32 origin_x = column * cell_width;
        const u32 origin_y = row * cell_height;
        BitmapFontRenderer::draw_cell(
            value, origin_x, origin_y, cell_width, cell_height, foreground, background,
            [](char ch, u32 glyph_y) { return BitmapFontRenderer::glyph_row(ch, glyph_y); },
            [](u32 x, u32 y, u32 color) { put_pixel(x, y, color); });
    }

    static usize append_literal(char *line, usize position, usize capacity, const char *text)
    {
        for (usize i = 0; text[i] != '\0' && position < capacity; ++i)
        {
            line[position++] = text[i];
        }
        return position;
    }

    static usize append_unsigned(char *line, usize position, usize capacity, u32 value)
    {
        char reversed[10]{};
        usize digits = 0;
        do
        {
            reversed[digits++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && digits < sizeof(reversed));
        while (digits > 0 && position < capacity)
        {
            line[position++] = reversed[--digits];
        }
        return position;
    }

    static void draw_mouse_status()
    {
        char line[text_columns + 1]{};
        usize position = 0;
        position = append_literal(line, position, text_columns, "mouse x=");
        position = append_unsigned(line, position, text_columns, pointer_x_);
        position = append_literal(line, position, text_columns, " y=");
        position = append_unsigned(line, position, text_columns, pointer_y_);
        position = append_literal(line, position, text_columns, " left=");
        if (position < text_columns)
        {
            line[position++] = pointer_left_button_ ? '1' : '0';
        }
        while (position < text_columns)
        {
            line[position++] = ' ';
        }

        const u32 foreground = pointer_left_button_ ? mouse_status_pressed_foreground : mouse_status_foreground;
        for (u32 column = 0; column < text_columns; ++column)
        {
            draw_cell(column, mouse_status_row, line[column], foreground, mouse_status_background);
        }
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
    static inline bool pointer_left_button_{false};
    static inline bool pointer_drawn_{false};
    static inline bool gui_active_{false};
    static inline i32 gui_delta_x_remainder_{0};
    static inline i32 gui_delta_y_remainder_{0};
    static inline u32 pointer_saved_[pointer_pixels]{};
    alignas(4096) static inline u32 pixels_[framebuffer_pixels]{};
    alignas(16) static inline volatile DmaAccess dma_access_{};
    alignas(16) static inline RamFbConfig ramfb_config_{};
};

} // namespace ok::driver::qemu_virt
