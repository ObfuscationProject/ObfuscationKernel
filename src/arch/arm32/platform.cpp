#include "ok/core/types.hpp"
#include "ok/driver/qemu_virt/ramfb.hpp"
#include "ok/driver/qemu_virt/virtio_input.hpp"

namespace
{

constexpr ok::uptr pl011_base = 0x09000000;
constexpr ok::uptr fw_cfg_base = 0x09020000;
constexpr ok::uptr uart_dr = pl011_base + 0x00;
constexpr ok::uptr uart_fr = pl011_base + 0x18;
constexpr ok::u32 uart_fr_rxfe = 1u << 4;
constexpr ok::u32 uart_fr_txff = 1u << 5;
constexpr ok::uptr virtio_input_bases[] = {
    0x0a000000, 0x0a000200, 0x0a000400, 0x0a000600, 0x0a000800, 0x0a000a00, 0x0a000c00, 0x0a000e00,
    0x0a001000, 0x0a001200, 0x0a001400, 0x0a001600, 0x0a001800, 0x0a001a00, 0x0a001c00, 0x0a001e00,
    0x0a002000, 0x0a002200, 0x0a002400, 0x0a002600, 0x0a002800, 0x0a002a00, 0x0a002c00, 0x0a002e00,
    0x0a003000, 0x0a003200, 0x0a003400, 0x0a003600, 0x0a003800, 0x0a003a00, 0x0a003c00, 0x0a003e00,
};
using RamFb = ok::driver::qemu_virt::RamFbConsole<fw_cfg_base>;
ok::driver::qemu_virt::VirtioInputDevice virtio_keyboard;
ok::driver::qemu_virt::VirtioInputDevice virtio_mouse;
bool virtio_left_shift = false;
bool virtio_right_shift = false;
bool virtio_mouse_left = false;
char pending_keyboard_input[3]{};
ok::usize pending_keyboard_input_size = 0;
ok::usize pending_keyboard_input_cursor = 0;

volatile ok::u32 &mmio32(ok::uptr address)
{
    return *reinterpret_cast<volatile ok::u32 *>(address);
}

void uart_write_char(char value)
{
    if (value == '\n')
    {
        uart_write_char('\r');
    }
    while ((mmio32(uart_fr) & uart_fr_txff) != 0)
    {
    }
    mmio32(uart_dr) = static_cast<ok::u32>(static_cast<unsigned char>(value));
}

void poll_virtio_mouse()
{
    virtio_mouse.initialize(virtio_input_bases, sizeof(virtio_input_bases) / sizeof(virtio_input_bases[0]),
                            ok::driver::qemu_virt::VirtioInputKind::mouse);
    ok::driver::qemu_virt::VirtioInputEvent event{};
    ok::i32 dx = 0;
    ok::i32 dy = 0;
    bool changed = false;
    while (virtio_mouse.poll(event))
    {
        if (event.type == ok::driver::qemu_virt::input_event_relative)
        {
            const auto value = static_cast<ok::i32>(event.value);
            if (event.code == ok::driver::qemu_virt::input_relative_x)
            {
                dx += value;
                changed = true;
            }
            else if (event.code == ok::driver::qemu_virt::input_relative_y)
            {
                dy += value;
                changed = true;
            }
        }
        else if (event.type == ok::driver::qemu_virt::input_event_key &&
                 event.code == ok::driver::qemu_virt::input_button_left)
        {
            virtio_mouse_left = event.value != 0;
            changed = true;
        }
    }
    if (changed)
    {
        RamFb::move_pointer(dx, dy, virtio_mouse_left);
    }
}

void queue_keyboard_escape(char final)
{
    pending_keyboard_input[0] = 0x1b;
    pending_keyboard_input[1] = '[';
    pending_keyboard_input[2] = final;
    pending_keyboard_input_size = 3;
    pending_keyboard_input_cursor = 0;
}

int pop_pending_keyboard_input()
{
    if (pending_keyboard_input_cursor >= pending_keyboard_input_size)
    {
        pending_keyboard_input_size = 0;
        pending_keyboard_input_cursor = 0;
        return -1;
    }
    return static_cast<unsigned char>(pending_keyboard_input[pending_keyboard_input_cursor++]);
}

int poll_virtio_keyboard()
{
    if (const int pending = pop_pending_keyboard_input(); pending >= 0)
    {
        return pending;
    }
    virtio_keyboard.initialize(virtio_input_bases, sizeof(virtio_input_bases) / sizeof(virtio_input_bases[0]),
                               ok::driver::qemu_virt::VirtioInputKind::keyboard);
    ok::driver::qemu_virt::VirtioInputEvent event{};
    while (virtio_keyboard.poll(event))
    {
        if (event.type != ok::driver::qemu_virt::input_event_key)
        {
            continue;
        }
        const bool pressed = event.value != 0;
        if (event.code == ok::driver::qemu_virt::input_key_left_shift)
        {
            virtio_left_shift = pressed;
            continue;
        }
        if (event.code == ok::driver::qemu_virt::input_key_right_shift)
        {
            virtio_right_shift = pressed;
            continue;
        }
        if (!pressed)
        {
            continue;
        }
        if (event.code == ok::driver::qemu_virt::input_key_up)
        {
            queue_keyboard_escape('A');
            return pop_pending_keyboard_input();
        }
        if (event.code == ok::driver::qemu_virt::input_key_down)
        {
            queue_keyboard_escape('B');
            return pop_pending_keyboard_input();
        }
        const char value =
            ok::driver::qemu_virt::map_linux_key_code(event.code, virtio_left_shift || virtio_right_shift);
        if (value != 0)
        {
            return static_cast<unsigned char>(value);
        }
    }
    return -1;
}

} // namespace

extern "C" void ok_platform_console_init() {}

extern "C" void ok_platform_console_write_char(char value)
{
    uart_write_char(value);
}

extern "C" void ok_platform_display_clear()
{
    RamFb::clear();
}

extern "C" void ok_platform_display_write_char(char value)
{
    RamFb::write_char(value);
}

extern "C" void ok_platform_display_gui_pixel(ok::u32 logical_width, ok::u32 logical_height, ok::u32 x, ok::u32 y,
                                              ok::u32 color)
{
    RamFb::draw_gui_pixel(logical_width, logical_height, x, y, color);
}

extern "C" void ok_platform_display_debug_marker()
{
    RamFb::draw_debug_marker();
}

extern "C" void ok_platform_debug_exit(ok::u32) {}

extern "C" void ok_platform_halt()
{
    asm volatile("wfi" ::: "memory");
}

extern "C" int ok_platform_input_poll()
{
    poll_virtio_mouse();
    if (const int input = poll_virtio_keyboard(); input >= 0)
    {
        return input;
    }
    if ((mmio32(uart_fr) & uart_fr_rxfe) != 0)
    {
        return -1;
    }
    return static_cast<int>(mmio32(uart_dr) & 0xffu);
}
