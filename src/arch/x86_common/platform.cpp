#include "ok/core/types.hpp"
#include "../qemu_virt/ramfb.hpp"

namespace
{

constexpr ok::u16 com1 = 0x3f8;
constexpr ok::u16 debug_exit_port = 0x00f4;
constexpr ok::uptr fw_cfg_io_base = 0x510;
constexpr ok::usize vga_columns = 80;
constexpr ok::usize vga_rows = 25;
constexpr ok::u16 keyboard_data_port = 0x60;
constexpr ok::u16 keyboard_status_port = 0x64;
using RamFb = ok::platform::qemu_virt::RamFbConsole<fw_cfg_io_base, true>;

ok::usize vga_row = 0;
ok::usize vga_column = 0;
bool left_shift = false;
bool right_shift = false;

void outb(ok::u16 port, ok::u8 value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

void outl(ok::u16 port, ok::u32 value)
{
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

ok::u8 inb(ok::u16 port)
{
    ok::u8 value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

void serial_write_char(char value)
{
    if (value == '\n')
    {
        serial_write_char('\r');
    }
    if (value == '\b')
    {
        constexpr char erase[] = {'\b', ' ', '\b'};
        for (const char ch : erase)
        {
            for (ok::usize attempt = 0; attempt < 100000; ++attempt)
            {
                if ((inb(com1 + 5) & 0x20u) != 0)
                {
                    break;
                }
            }
            outb(com1, static_cast<ok::u8>(ch));
        }
        return;
    }
    for (ok::usize attempt = 0; attempt < 100000; ++attempt)
    {
        if ((inb(com1 + 5) & 0x20u) != 0)
        {
            break;
        }
    }
    outb(com1, static_cast<ok::u8>(value));
}

volatile ok::u16 *vga_buffer()
{
    return reinterpret_cast<volatile ok::u16 *>(0xb8000);
}

void vga_newline()
{
    vga_column = 0;
    if (vga_row + 1 < vga_rows)
    {
        ++vga_row;
        return;
    }

    auto *buffer = vga_buffer();
    for (ok::usize row = 1; row < vga_rows; ++row)
    {
        for (ok::usize column = 0; column < vga_columns; ++column)
        {
            buffer[(row - 1) * vga_columns + column] = buffer[row * vga_columns + column];
        }
    }
    for (ok::usize column = 0; column < vga_columns; ++column)
    {
        buffer[(vga_rows - 1) * vga_columns + column] = static_cast<ok::u16>(0x0f00u | ' ');
    }
}

void vga_write_char(char value)
{
    if (value == '\n')
    {
        vga_newline();
        return;
    }
    if (value == '\r')
    {
        vga_column = 0;
        return;
    }
    if (value == '\b')
    {
        if (vga_column > 0)
        {
            --vga_column;
        }
        vga_buffer()[vga_row * vga_columns + vga_column] = static_cast<ok::u16>(0x0f00u | ' ');
        return;
    }

    auto *buffer = vga_buffer();
    buffer[vga_row * vga_columns + vga_column] = static_cast<ok::u16>(0x0f00u | static_cast<ok::u8>(value));
    ++vga_column;
    if (vga_column == vga_columns)
    {
        vga_newline();
    }
}

void vga_debug_marker()
{
    auto *buffer = vga_buffer();
    constexpr ok::usize marker_width = 9;
    constexpr ok::usize marker_height = 3;
    constexpr ok::usize left = vga_columns - marker_width - 2;
    constexpr ok::usize top = vga_rows - marker_height - 1;
    constexpr ok::u8 colors[] = {0x1e, 0x2b, 0x3d, 0x4f, 0x5e, 0x6b, 0x9f, 0xb0, 0xe4};
    for (ok::usize row = 0; row < marker_height; ++row)
    {
        for (ok::usize column = 0; column < marker_width; ++column)
        {
            const auto color = colors[(row + column) % (sizeof(colors) / sizeof(colors[0]))];
            buffer[(top + row) * vga_columns + left + column] =
                static_cast<ok::u16>((static_cast<ok::u16>(color) << 8) | 0xdbu);
        }
    }
}

char map_scancode(ok::u8 scancode)
{
    constexpr char normal[] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8',
        '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
        't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
        '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
        'm', ',', '.', '/', 0, '*', 0, ' ',
    };
    constexpr char shifted[] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*',
        '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
        'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
        '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N',
        'M', '<', '>', '?', 0, '*', 0, ' ',
    };
    if (scancode >= sizeof(normal))
    {
        return 0;
    }
    return (left_shift || right_shift) ? shifted[scancode] : normal[scancode];
}

} // namespace

extern "C" void ok_platform_console_init()
{
    outb(com1 + 1, 0x00);
    outb(com1 + 3, 0x80);
    outb(com1 + 0, 0x03);
    outb(com1 + 1, 0x00);
    outb(com1 + 3, 0x03);
    outb(com1 + 2, 0xc7);
    outb(com1 + 4, 0x0b);
}

extern "C" void ok_platform_console_write_char(char value)
{
    serial_write_char(value);
}

extern "C" void ok_platform_display_clear()
{
    if (RamFb::available())
    {
        RamFb::clear();
        return;
    }
    auto *buffer = vga_buffer();
    for (ok::usize i = 0; i < vga_columns * vga_rows; ++i)
    {
        buffer[i] = static_cast<ok::u16>(0x0f00u | ' ');
    }
    vga_row = 0;
    vga_column = 0;
}

extern "C" void ok_platform_display_write_char(char value)
{
    if (RamFb::available())
    {
        RamFb::write_char(value);
        return;
    }
    vga_write_char(value);
}

extern "C" void ok_platform_display_debug_marker()
{
    if (RamFb::available())
    {
        RamFb::draw_debug_marker();
        return;
    }
    vga_debug_marker();
}

extern "C" void ok_platform_debug_exit(ok::u32 code)
{
    outl(debug_exit_port, code);
}

extern "C" void ok_platform_halt()
{
    asm volatile("hlt" ::: "memory");
}

extern "C" int ok_platform_input_poll()
{
    const auto status = inb(keyboard_status_port);
    if ((status & 0x01u) == 0)
    {
        return -1;
    }
    if ((status & 0x20u) != 0)
    {
        static_cast<void>(inb(keyboard_data_port));
        return -1;
    }

    const auto scancode = inb(keyboard_data_port);
    const bool released = (scancode & 0x80u) != 0;
    const auto key = static_cast<ok::u8>(scancode & 0x7fu);
    if (key == 0x2a)
    {
        left_shift = !released;
        return -1;
    }
    if (key == 0x36)
    {
        right_shift = !released;
        return -1;
    }
    if (released)
    {
        return -1;
    }

    const char value = map_scancode(key);
    return value == 0 ? -1 : static_cast<unsigned char>(value);
}
