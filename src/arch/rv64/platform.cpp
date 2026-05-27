#include "ok/core/types.hpp"
#include "ok/core/entry.hpp"
#include "ok/driver/qemu_virt/ramfb.hpp"
#include "ok/driver/qemu_virt/virtio_input.hpp"

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
using RamFb = ok::driver::qemu_virt::RamFbConsole<fw_cfg_base>;
ok::driver::qemu_virt::VirtioInputDevice virtio_keyboard;
ok::driver::qemu_virt::VirtioInputDevice virtio_mouse;
bool virtio_left_shift = false;
bool virtio_right_shift = false;
bool virtio_left_control = false;
bool virtio_right_control = false;
bool virtio_mouse_left = false;
char pending_keyboard_input[3]{};
ok::usize pending_keyboard_input_size = 0;
ok::usize pending_keyboard_input_cursor = 0;

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

void flush_virtio_mouse_motion(ok::i32 &dx, ok::i32 &dy, bool &changed)
{
    if (!changed)
    {
        return;
    }
    RamFb::move_pointer(dx, dy, virtio_mouse_left);
    static_cast<void>(ok::ok_gui_mouse_position_event(RamFb::gui_pointer_x(), RamFb::gui_pointer_y(),
                                                      virtio_mouse_left));
    RamFb::redraw_pointer_after_gui_present();
    dx = 0;
    dy = 0;
    changed = false;
}

void poll_virtio_mouse()
{
    virtio_mouse.initialize(virtio_input_bases, sizeof(virtio_input_bases) / sizeof(virtio_input_bases[0]),
                            ok::driver::qemu_virt::VirtioInputKind::mouse);
    ok::driver::qemu_virt::VirtioInputEvent event{};
    ok::i32 dx = 0;
    ok::i32 dy = 0;
    ok::i32 wheel = 0;
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
            else if (event.code == ok::driver::qemu_virt::input_relative_wheel)
            {
                wheel += value;
            }
        }
        else if (event.type == ok::driver::qemu_virt::input_event_key &&
                 event.code == ok::driver::qemu_virt::input_button_left)
        {
            flush_virtio_mouse_motion(dx, dy, changed);
            virtio_mouse_left = event.value != 0;
            RamFb::move_pointer(0, 0, virtio_mouse_left);
            static_cast<void>(ok::ok_gui_mouse_position_event(RamFb::gui_pointer_x(), RamFb::gui_pointer_y(),
                                                              virtio_mouse_left));
            RamFb::redraw_pointer_after_gui_present();
        }
    }
    flush_virtio_mouse_motion(dx, dy, changed);
    if (wheel != 0)
    {
        static_cast<void>(ok::ok_debug_shell_scroll_gui(-wheel));
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
        if (event.code == ok::driver::qemu_virt::input_key_left_ctrl)
        {
            virtio_left_control = pressed;
            continue;
        }
        if (event.code == ok::driver::qemu_virt::input_key_right_ctrl)
        {
            virtio_right_control = pressed;
            continue;
        }
        if (!pressed)
        {
            continue;
        }
        if (event.code == ok::driver::qemu_virt::input_key_f1)
        {
            return ok::ok_input_open_file_manager;
        }
        if (event.code == ok::driver::qemu_virt::input_key_f12)
        {
            return ok::ok_input_open_shell;
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
            ok::driver::qemu_virt::map_linux_key_code(event.code, virtio_left_shift || virtio_right_shift,
                                                      virtio_left_control || virtio_right_control);
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

extern "C" void ok_platform_display_gui_frame_begin(ok::u32, ok::u32)
{
    RamFb::begin_gui_frame();
}

extern "C" void ok_platform_display_gui_frame(ok::u32 logical_width, ok::u32 logical_height, const ok::u32 *pixels,
                                              ok::usize pixel_count)
{
    RamFb::draw_gui_frame(logical_width, logical_height, pixels, pixel_count);
}

extern "C" void ok_platform_display_gui_frame_end(ok::u32, ok::u32)
{
    RamFb::end_gui_frame();
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

extern "C" void ok_platform_poll_idle()
{
    asm volatile("nop" ::: "memory");
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
