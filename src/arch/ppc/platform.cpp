#include "ok/core/types.hpp"

namespace
{

constexpr ok::uptr uart_primary_base = 0xe0004500;
constexpr ok::uptr uart_secondary_base = 0xe0004600;
constexpr ok::u8 uart_lsr_data_ready = 1u << 0;
constexpr ok::u8 uart_lsr_tx_empty = 1u << 5;
bool uart_secondary_enabled = true;

volatile ok::u8 &mmio8(ok::uptr address)
{
    return *reinterpret_cast<volatile ok::u8 *>(address);
}

void uart_write_char_at(ok::uptr base, char value, bool required)
{
    const auto lsr = base + 0x05;
    if (!required && !uart_secondary_enabled)
    {
        return;
    }
    if (required)
    {
        while ((mmio8(lsr) & uart_lsr_tx_empty) == 0)
        {
        }
    }
    else
    {
        bool ready = false;
        for (ok::usize attempt = 0; attempt < 100000; ++attempt)
        {
            if ((mmio8(lsr) & uart_lsr_tx_empty) != 0)
            {
                ready = true;
                break;
            }
        }
        if (!ready)
        {
            uart_secondary_enabled = false;
            return;
        }
    }
    mmio8(base) = static_cast<ok::u8>(value);
}

void uart_write_char(char value)
{
    if (value == '\n')
    {
        uart_write_char('\r');
    }
    uart_write_char_at(uart_primary_base, value, true);
    uart_write_char_at(uart_secondary_base, value, false);
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

extern "C" void ok_platform_poll_idle()
{
    asm volatile("nop" ::: "memory");
}

extern "C" int ok_platform_input_poll()
{
    constexpr ok::uptr uart_lsr = uart_primary_base + 0x05;
    if ((mmio8(uart_lsr) & uart_lsr_data_ready) == 0)
    {
        return -1;
    }
    return static_cast<int>(mmio8(uart_primary_base));
}
