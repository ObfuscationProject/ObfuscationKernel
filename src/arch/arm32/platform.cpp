#include "ok/core/types.hpp"
#include "ok/core/entry.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/driver/qemu_virt/ramfb.hpp"
#include "ok/driver/qemu_virt/virtio_input.hpp"
#include "../../interrupt/console_ring.hpp"

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
bool virtio_left_control = false;
bool virtio_right_control = false;
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

// Keyboard and mouse events arrive on their virtio-mmio lines; the loop reads
// the queues only after an interrupt, until both are drained.
void input_interrupt(void *device, ok::interrupt::IrqLine)
{
    static_cast<ok::driver::qemu_virt::VirtioInputDevice *>(device)->acknowledge();
    ok::interrupt::post_event(ok::interrupt::EventSource::input);
}

// An absent device leaves nothing to poll; a present one needs its line.
bool bind_input(ok::driver::qemu_virt::VirtioInputDevice &device, ok::driver::qemu_virt::VirtioInputKind kind)
{
    if (!device.initialize(virtio_input_bases, sizeof(virtio_input_bases) / sizeof(virtio_input_bases[0]), kind))
        return true;
    const auto line = ok::interrupt::virtio_mmio_line((device.base() - virtio_input_bases[0]) / 0x200);
    return line && ok::interrupt::bind_platform_irq(line.value().line, line.value().trigger, input_interrupt, &device);
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

// Runs when the loop serves the input source; posts it again until both
// queues and the pending key sequence are drained.
extern "C" int ok_platform_input_poll()
{
    poll_virtio_mouse();
    const int key = poll_virtio_keyboard();
    if (key >= 0)
        ok::interrupt::post_event(ok::interrupt::EventSource::input);
    return key;
}

namespace
{
// PL011 receive bytes the SPI 1 (ID 33) bottom half moved out of the UART.
constexpr ok::interrupt::IrqLine uart_irq = 33;
constexpr ok::uptr uart_imsc = pl011_base + 0x38;
constexpr ok::uptr uart_icr = pl011_base + 0x44;
constexpr ok::u32 uart_receive_interrupts = (1u << 4) | (1u << 6); // RX and receive timeout.
constinit ok::interrupt::ConsoleRing<256> uart_ring{};

int read_uart()
{
    if ((mmio32(uart_fr) & uart_fr_rxfe) != 0)
        return -1;
    return static_cast<int>(mmio32(uart_dr) & 0xffu);
}

void uart_interrupt(void *, ok::interrupt::IrqLine)
{
    uart_ring.fill(read_uart);
    mmio32(uart_icr) = uart_receive_interrupts;
}
} // namespace

extern "C" ok::Status ok_platform_bind_irqs()
{
    if (bind_input(virtio_keyboard, ok::driver::qemu_virt::VirtioInputKind::keyboard) &&
        bind_input(virtio_mouse, ok::driver::qemu_virt::VirtioInputKind::mouse))
    {
        ok::interrupt::drive_events(ok::interrupt::EventSource::input);
        // Events queued before the lines were bound.
        ok::interrupt::post_event(ok::interrupt::EventSource::input);
    }
    if (auto status = ok::interrupt::bind_platform_irq(uart_irq, ok::interrupt::IrqTrigger::level, uart_interrupt,
                                                       nullptr);
        !status.ok())
        return status;
    mmio32(uart_icr) = uart_receive_interrupts;
    mmio32(uart_imsc) = mmio32(uart_imsc) | uart_receive_interrupts;
    ok::interrupt::drive_events(ok::interrupt::EventSource::console);
    return ok::Status::success();
}

extern "C" int ok_platform_console_poll()
{
    return uart_ring.take(read_uart);
}
