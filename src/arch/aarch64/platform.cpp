#include "ok/core/types.hpp"
#include "../qemu_virt/ramfb.hpp"

namespace
{

constexpr ok::uptr pl011_base = 0x09000000;
constexpr ok::uptr fw_cfg_base = 0x09020000;
constexpr ok::uptr uart_dr = pl011_base + 0x00;
constexpr ok::uptr uart_fr = pl011_base + 0x18;
constexpr ok::u32 uart_fr_rxfe = 1u << 4;
constexpr ok::u32 uart_fr_txff = 1u << 5;
using RamFb = ok::platform::qemu_virt::RamFbConsole<fw_cfg_base>;

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
    asm volatile("wfe" ::: "memory");
}

extern "C" int ok_platform_input_poll()
{
    if ((mmio32(uart_fr) & uart_fr_rxfe) != 0)
    {
        return -1;
    }
    return static_cast<int>(mmio32(uart_dr) & 0xffu);
}
