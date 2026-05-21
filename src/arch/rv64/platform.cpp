#include "ok/core/types.hpp"
#include "../qemu_virt/ramfb.hpp"
#include "../qemu_virt/virtio_input.hpp"

namespace
{

constexpr ok::uptr uart_base = 0x10000000;
constexpr ok::uptr fw_cfg_base = 0x10100000;
constexpr ok::uptr uart_rbr_thr = uart_base + 0x00;
constexpr ok::uptr uart_lsr = uart_base + 0x05;
constexpr ok::u8 uart_lsr_data_ready = 1u << 0;
constexpr ok::u8 uart_lsr_tx_empty = 1u << 5;
constexpr ok::uptr virtio_input_bases[] = {
    0x10001000, 0x10002000, 0x10003000, 0x10004000, 0x10005000, 0x10006000, 0x10007000, 0x10008000,
};
using RamFb = ok::platform::qemu_virt::RamFbConsole<fw_cfg_base>;
ok::platform::qemu_virt::VirtioInputDevice virtio_keyboard;
ok::platform::qemu_virt::VirtioInputDevice virtio_mouse;
bool virtio_left_shift = false;
bool virtio_right_shift = false;
bool virtio_mouse_left = false;

volatile ok::u8 &mmio8(ok::uptr address)
{
    return *reinterpret_cast<volatile ok::u8 *>(address);
}

void uart_write_char(char value)
{
    if (value == '\n')
    {
        uart_write_char('\r');
    }
    while ((mmio8(uart_lsr) & uart_lsr_tx_empty) == 0)
    {
    }
    mmio8(uart_rbr_thr) = static_cast<ok::u8>(value);
}

void poll_virtio_mouse()
{
    virtio_mouse.initialize(virtio_input_bases, sizeof(virtio_input_bases) / sizeof(virtio_input_bases[0]), 1);
    ok::platform::qemu_virt::VirtioInputEvent event{};
    ok::i32 dx = 0;
    ok::i32 dy = 0;
    bool changed = false;
    while (virtio_mouse.poll(event))
    {
        if (event.type == ok::platform::qemu_virt::input_event_relative)
        {
            const auto value = static_cast<ok::i32>(event.value);
            if (event.code == ok::platform::qemu_virt::input_relative_x)
            {
                dx += value;
                changed = true;
            }
            else if (event.code == ok::platform::qemu_virt::input_relative_y)
            {
                dy += value;
                changed = true;
            }
        }
        else if (event.type == ok::platform::qemu_virt::input_event_key &&
                 event.code == ok::platform::qemu_virt::input_button_left)
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

int poll_virtio_keyboard()
{
    virtio_keyboard.initialize(virtio_input_bases, sizeof(virtio_input_bases) / sizeof(virtio_input_bases[0]), 0);
    ok::platform::qemu_virt::VirtioInputEvent event{};
    while (virtio_keyboard.poll(event))
    {
        if (event.type != ok::platform::qemu_virt::input_event_key)
        {
            continue;
        }
        const bool pressed = event.value != 0;
        if (event.code == ok::platform::qemu_virt::input_key_left_shift)
        {
            virtio_left_shift = pressed;
            continue;
        }
        if (event.code == ok::platform::qemu_virt::input_key_right_shift)
        {
            virtio_right_shift = pressed;
            continue;
        }
        if (!pressed)
        {
            continue;
        }
        const char value =
            ok::platform::qemu_virt::map_linux_key_code(event.code, virtio_left_shift || virtio_right_shift);
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
    if ((mmio8(uart_lsr) & uart_lsr_data_ready) == 0)
    {
        return -1;
    }
    return static_cast<int>(mmio8(uart_rbr_thr));
}
