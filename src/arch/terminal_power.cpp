#include "ok/arch/terminal_power.hpp"
#include "ok/arch/pc8042_reset.hpp"
#include "ok/arch/piix4_pm_poweroff.hpp"

#if defined(OK_NATIVE_PC8042_REBOOT)
#if OK_NATIVE_PC8042_REBOOT != 1 || !defined(OK_ARCH_TARGET_X86_64) || !defined(__x86_64__)
#error "PC/8042 reboot requires the explicit x86_64 policy value 1"
#endif
#endif

#if defined(OK_NATIVE_QEMU_PIIX4_POWEROFF)
#if OK_NATIVE_QEMU_PIIX4_POWEROFF != 1 || !defined(OK_ARCH_TARGET_X86_64) || !defined(__x86_64__)
#error "QEMU PIIX4 PM poweroff requires the explicit x86_64 policy value 1"
#endif
// These exact native instruction labels allow read-only execution observation.
extern "C" void ok_arch_piix4_pm_control_write(ok::u16, ok::u16) noexcept;
asm(".pushsection .text.ok_arch_piix4_pm_control_write,\"ax\",@progbits\n"
    ".globl ok_arch_piix4_pm_control_write\n"
    ".type ok_arch_piix4_pm_control_write,@function\n"
    "ok_arch_piix4_pm_control_write:\n"
    "movw %di, %dx\n"
    "movw %si, %ax\n"
    ".globl ok_arch_piix4_pm_control_outw\n"
    "ok_arch_piix4_pm_control_outw:\n"
    "outw %ax, %dx\n"
    "ret\n"
    ".size ok_arch_piix4_pm_control_write,.-ok_arch_piix4_pm_control_write\n"
    ".popsection\n");
#endif

#if defined(__x86_64__)
// Keep one exact allocated loop for ordinary HALT and reset-failure observation.
extern "C" [[noreturn]] void ok_arch_terminal_hold() noexcept;
asm(".pushsection .text.ok_arch_terminal_hold,\"ax\",@progbits\n"
    ".globl ok_arch_terminal_hold\n"
    ".type ok_arch_terminal_hold,@function\n"
    "ok_arch_terminal_hold:\n"
    "cli\n"
    "hlt\n"
    "jmp ok_arch_terminal_hold\n"
    ".size ok_arch_terminal_hold,.-ok_arch_terminal_hold\n"
    ".popsection\n");
#endif

namespace ok::arch
{
namespace
{

template <Architecture> struct TerminalBackend
{
    static constexpr bool halt_supported = false;
    static constexpr bool reboot_supported = false;
    static constexpr bool poweroff_supported = false;
    static Status preflight_poweroff() noexcept { return Status::unsupported("no poweroff backend"); }
    static Status request_poweroff() noexcept { return Status::unsupported("no poweroff backend"); }
    static Status preflight_reboot() noexcept { return Status::unsupported("no reboot backend"); }
    static Status request_reboot() noexcept { return Status::unsupported("no reboot backend"); }
    static void disable_interrupts() noexcept {}
    static void hold_once() noexcept
    {
        asm volatile("" ::: "memory");
    }
    [[noreturn]] static void hold_forever() noexcept { detail::terminal_hold<TerminalBackend>(); }
};

#if defined(__x86_64__)
template <> struct TerminalBackend<Architecture::x86_64>
{
    static constexpr bool halt_supported = true;
#if defined(OK_NATIVE_PC8042_REBOOT)
    static constexpr bool reboot_supported = true;
#else
    static constexpr bool reboot_supported = false;
#endif
#if defined(OK_NATIVE_QEMU_PIIX4_POWEROFF)
    static constexpr bool poweroff_supported = true;
    struct Configuration
    {
        static u32 read_configuration(u8 slot, u8 function, u8 offset) noexcept
        {
            // Local IRQ closure serializes each CF8/CFC pair on this one-CPU profile.
            // This does not supply a cross-CPU PCI configuration lock.
            u64 flags;
            asm volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
            u32 previous;
            asm volatile("inl %1, %0" : "=a"(previous) : "Nd"(u16{0xcf8}) : "memory");
            const u32 address = 0x80000000u | (u32{slot} << 11) | (u32{function} << 8) | (offset & 0xfcu);
            asm volatile("outl %0, %1" : : "a"(address), "Nd"(u16{0xcf8}) : "memory");
            u32 value;
            asm volatile("inl %1, %0" : "=a"(value) : "Nd"(u16{0xcfc}) : "memory");
            asm volatile("outl %0, %1" : : "a"(previous), "Nd"(u16{0xcf8}) : "memory");
            asm volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
            return value;
        }
    };
    struct Control
    {
        static u16 read_control(u16 port) noexcept
        {
            u16 value;
            asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
            return value;
        }
        static void write_control(u16 port, u16 value) noexcept
        {
            ok_arch_piix4_pm_control_write(port, value);
        }
    };
    static Status preflight_poweroff() noexcept
    {
        return detail::Piix4PmPoweroff<Configuration, Control>::preflight();
    }
    static Status request_poweroff() noexcept
    {
        return detail::Piix4PmPoweroff<Configuration, Control>::request();
    }
#else
    static constexpr bool poweroff_supported = false;
    static Status preflight_poweroff() noexcept { return Status::unsupported("no selected QEMU PIIX4 PM backend"); }
    static Status request_poweroff() noexcept { return Status::unsupported("no selected QEMU PIIX4 PM backend"); }
#endif
    struct Ports
    {
        static u8 read_status() noexcept
        {
            u8 value;
            asm volatile("inb %1, %0" : "=a"(value) : "Nd"(u16{0x64}) : "memory");
            return value;
        }
        static void write_command(u8 value) noexcept
        {
            asm volatile("outb %0, %1" : : "a"(value), "Nd"(u16{0x64}) : "memory");
        }
        static void pause() noexcept { asm volatile("pause" ::: "memory"); }
    };
    static Status preflight_reboot() noexcept { return detail::Pc8042Reset<Ports>::preflight(); }
    static Status request_reboot() noexcept { return detail::Pc8042Reset<Ports>::request(); }
    static void disable_interrupts() noexcept
    {
        asm volatile("cli" ::: "memory");
    }
    [[noreturn]] static void hold_forever() noexcept { ok_arch_terminal_hold(); }
    static void hold_once() noexcept
    {
        asm volatile("hlt" ::: "memory");
    }
};
#endif

using SelectedBackend = TerminalBackend<configured_architecture()>;
static_assert(detail::TerminalPowerBackend<SelectedBackend>);

} // namespace

bool terminal_power_action_supported(SystemPowerAction action) noexcept
{
    return detail::terminal_supported<SelectedBackend>(action);
}

Status preflight_terminal_power_action(SystemPowerAction action) noexcept
{
    return detail::terminal_preflight<SelectedBackend>(action);
}

Status close_shutdown_interrupt_admission() noexcept
{
    if constexpr (!SelectedBackend::halt_supported)
        return Status::unsupported("shutdown interrupt closure is not supported by this architecture");
    SelectedBackend::disable_interrupts();
    return Status::success();
}

[[noreturn]] void terminal_power_action(SystemPowerAction action) noexcept
{
    detail::terminal_dispatch<SelectedBackend>(action);
}

} // namespace ok::arch
