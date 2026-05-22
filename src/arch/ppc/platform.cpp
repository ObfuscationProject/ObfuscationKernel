#include "ok/core/types.hpp"

namespace
{

constexpr ok::uptr uart_base = 0xe0004500;
constexpr ok::uptr uart_rbr_thr = uart_base + 0x00;
constexpr ok::uptr uart_lsr = uart_base + 0x05;
constexpr ok::u8 uart_lsr_data_ready = 1u << 0;
constexpr ok::u8 uart_lsr_tx_empty = 1u << 5;

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

} // namespace

extern "C" void ok_platform_console_init() {}

extern "C" void ok_platform_console_write_char(char value)
{
    uart_write_char(value);
}

extern "C" void ok_platform_display_clear() {}

extern "C" void ok_platform_display_write_char(char) {}

extern "C" void ok_platform_display_gui_pixel(ok::u32, ok::u32, ok::u32, ok::u32, ok::u32) {}

extern "C" void ok_platform_display_debug_marker() {}

extern "C" void ok_platform_debug_exit(ok::u32) {}

extern "C" void ok_platform_halt()
{
    asm volatile("nop" ::: "memory");
}

extern "C" int ok_platform_input_poll()
{
    if ((mmio8(uart_lsr) & uart_lsr_data_ready) == 0)
    {
        return -1;
    }
    return static_cast<int>(mmio8(uart_rbr_thr));
}
