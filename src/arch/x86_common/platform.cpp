#include "ok/core/types.hpp"
#include "ok/core/entry.hpp"
#include "ok/driver/qemu_virt/ramfb.hpp"

namespace
{

constexpr ok::u16 com1 = 0x3f8;
constexpr ok::u16 debug_exit_port = 0x00f4;
constexpr ok::uptr fw_cfg_io_base = 0x510;
constexpr ok::usize vga_columns = 80;
constexpr ok::usize vga_rows = 25;
constexpr ok::u16 keyboard_data_port = 0x60;
constexpr ok::u16 keyboard_status_port = 0x64;
constexpr ok::u8 ps2_status_output_full = 1u << 0;
constexpr ok::u8 ps2_status_input_full = 1u << 1;
constexpr ok::u8 ps2_status_aux_output = 1u << 5;
constexpr ok::u8 ps2_config_aux_irq = 1u << 1;
constexpr ok::u8 ps2_config_aux_clock_disabled = 1u << 5;
constexpr ok::u8 ps2_command_read_config = 0x20;
constexpr ok::u8 ps2_command_write_config = 0x60;
constexpr ok::u8 ps2_command_enable_aux = 0xa8;
constexpr ok::u8 ps2_command_write_aux = 0xd4;
constexpr ok::u8 ps2_mouse_ack = 0xfa;
constexpr ok::u8 ps2_mouse_get_id = 0xf2;
constexpr ok::u8 ps2_mouse_set_sample_rate = 0xf3;
constexpr ok::u8 ps2_mouse_set_defaults = 0xf6;
constexpr ok::u8 ps2_mouse_enable_streaming = 0xf4;
using RamFb = ok::driver::qemu_virt::RamFbConsole<fw_cfg_io_base, true>;

ok::usize vga_row = 0;
ok::usize vga_column = 0;
bool left_shift = false;
bool right_shift = false;
bool ps2_mouse_initialized = false;
ok::u8 ps2_mouse_packet[4]{};
ok::usize ps2_mouse_packet_index = 0;
ok::usize ps2_mouse_packet_size = 3;
char pending_input[3]{};
ok::usize pending_input_size = 0;
ok::usize pending_input_cursor = 0;
bool ps2_extended_scancode = false;

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

void queue_escape_sequence(char final)
{
    pending_input[0] = 0x1b;
    pending_input[1] = '[';
    pending_input[2] = final;
    pending_input_size = 3;
    pending_input_cursor = 0;
}

int pop_pending_input()
{
    if (pending_input_cursor >= pending_input_size)
    {
        pending_input_size = 0;
        pending_input_cursor = 0;
        return -1;
    }
    return static_cast<unsigned char>(pending_input[pending_input_cursor++]);
}

bool ps2_wait_input_clear()
{
    for (ok::usize attempt = 0; attempt < 100000; ++attempt)
    {
        if ((inb(keyboard_status_port) & ps2_status_input_full) == 0)
        {
            return true;
        }
    }
    return false;
}

bool ps2_wait_output_full()
{
    for (ok::usize attempt = 0; attempt < 100000; ++attempt)
    {
        if ((inb(keyboard_status_port) & ps2_status_output_full) != 0)
        {
            return true;
        }
    }
    return false;
}

bool ps2_read_byte(ok::u8 &value)
{
    if (!ps2_wait_output_full())
    {
        return false;
    }
    value = inb(keyboard_data_port);
    return true;
}

bool ps2_write_controller(ok::u8 command)
{
    if (!ps2_wait_input_clear())
    {
        return false;
    }
    outb(keyboard_status_port, command);
    return true;
}

bool ps2_write_data(ok::u8 value)
{
    if (!ps2_wait_input_clear())
    {
        return false;
    }
    outb(keyboard_data_port, value);
    return true;
}

void ps2_flush_output()
{
    for (ok::usize attempt = 0; attempt < 32; ++attempt)
    {
        if ((inb(keyboard_status_port) & ps2_status_output_full) == 0)
        {
            return;
        }
        static_cast<void>(inb(keyboard_data_port));
    }
}

bool ps2_read_config(ok::u8 &config)
{
    if (!ps2_write_controller(ps2_command_read_config))
    {
        return false;
    }
    return ps2_read_byte(config);
}

bool ps2_write_config(ok::u8 config)
{
    return ps2_write_controller(ps2_command_write_config) && ps2_write_data(config);
}

bool ps2_write_mouse(ok::u8 command)
{
    if (!ps2_write_controller(ps2_command_write_aux) || !ps2_write_data(command))
    {
        return false;
    }
    ok::u8 ack = 0;
    return ps2_read_byte(ack) && ack == ps2_mouse_ack;
}

bool ps2_mouse_sample_rate(ok::u8 rate)
{
    return ps2_write_mouse(ps2_mouse_set_sample_rate) && ps2_write_mouse(rate);
}

bool ps2_mouse_id(ok::u8 &id)
{
    if (!ps2_write_mouse(ps2_mouse_get_id))
    {
        return false;
    }
    return ps2_read_byte(id);
}

void ps2_mouse_init()
{
    ps2_flush_output();
    static_cast<void>(ps2_write_controller(ps2_command_enable_aux));

    ok::u8 config = 0;
    if (ps2_read_config(config))
    {
        config = static_cast<ok::u8>(config & ~ps2_config_aux_irq);
        config = static_cast<ok::u8>(config & ~ps2_config_aux_clock_disabled);
        static_cast<void>(ps2_write_config(config));
    }

    static_cast<void>(ps2_write_mouse(ps2_mouse_set_defaults));
    ok::u8 mouse_id = 0;
    if (ps2_mouse_sample_rate(200) && ps2_mouse_sample_rate(100) && ps2_mouse_sample_rate(80) &&
        ps2_mouse_id(mouse_id) && mouse_id == 3)
    {
        ps2_mouse_packet_size = 4;
    }
    else
    {
        ps2_mouse_packet_size = 3;
    }
    static_cast<void>(ps2_write_mouse(ps2_mouse_enable_streaming));
    ps2_mouse_initialized = true;
    ps2_mouse_packet_index = 0;
}

void handle_ps2_mouse_byte(ok::u8 value)
{
    if (ps2_mouse_packet_index == 0 && (value & 0x08u) == 0)
    {
        return;
    }

    ps2_mouse_packet[ps2_mouse_packet_index++] = value;
    if (ps2_mouse_packet_index < ps2_mouse_packet_size)
    {
        return;
    }
    ps2_mouse_packet_index = 0;

    const ok::u8 header = ps2_mouse_packet[0];
    if ((header & 0xc0u) != 0)
    {
        return;
    }

    const auto dx = static_cast<ok::i32>(static_cast<ok::i8>(ps2_mouse_packet[1]));
    const auto dy = static_cast<ok::i32>(static_cast<ok::i8>(ps2_mouse_packet[2]));
    const bool left_button = (header & 0x01u) != 0;
    RamFb::move_pointer(dx, -dy, left_button);
    if (ps2_mouse_packet_size == 4)
    {
        const auto z = ps2_mouse_packet[3] & 0x0fu;
        const auto wheel = z >= 8 ? static_cast<ok::i32>(z) - 16 : static_cast<ok::i32>(z);
        if (wheel != 0)
        {
            static_cast<void>(ok::ok_debug_shell_scroll_gui(wheel));
        }
    }
}

void poll_ps2_mouse()
{
    if (!ps2_mouse_initialized)
    {
        return;
    }
    for (ok::usize attempt = 0; attempt < 32; ++attempt)
    {
        const auto status = inb(keyboard_status_port);
        if ((status & ps2_status_output_full) == 0 || (status & ps2_status_aux_output) == 0)
        {
            return;
        }
        handle_ps2_mouse_byte(inb(keyboard_data_port));
    }
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
    ps2_mouse_init();
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

extern "C" void ok_platform_display_gui_pixel(ok::u32 logical_width, ok::u32 logical_height, ok::u32 x, ok::u32 y,
                                              ok::u32 color)
{
    RamFb::draw_gui_pixel(logical_width, logical_height, x, y, color);
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
    if (const int pending = pop_pending_input(); pending >= 0)
    {
        return pending;
    }
    poll_ps2_mouse();
    const auto status = inb(keyboard_status_port);
    if ((status & ps2_status_output_full) == 0)
    {
        return -1;
    }
    const auto scancode = inb(keyboard_data_port);
    if ((status & ps2_status_aux_output) != 0)
    {
        handle_ps2_mouse_byte(scancode);
        return -1;
    }

    const bool released = (scancode & 0x80u) != 0;
    const auto key = static_cast<ok::u8>(scancode & 0x7fu);
    if (scancode == 0xe0)
    {
        ps2_extended_scancode = true;
        return -1;
    }
    if (ps2_extended_scancode)
    {
        ps2_extended_scancode = false;
        if (!released && key == 0x48)
        {
            queue_escape_sequence('A');
            return pop_pending_input();
        }
        if (!released && key == 0x50)
        {
            queue_escape_sequence('B');
            return pop_pending_input();
        }
        return -1;
    }
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
