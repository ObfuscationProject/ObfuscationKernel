// Hosted backend model and read-only capability queries; no privileged instructions execute.
#include "ok/arch/terminal_power.hpp"
#include "ok/arch/pc8042_reset.hpp"

#include <csetjmp>
#include <iostream>

namespace
{
std::jmp_buf escape;
unsigned disables{};
unsigned holds{};
bool ordered{true};
struct ReturningBackend
{
    static void disable_interrupts() noexcept
    {
        ordered &= disables == holds;
        ++disables;
    }
    static void hold_once() noexcept
    {
        ordered &= disables == holds + 1;
        if (++holds == 4) std::longjmp(escape, 1);
    }
};
static_assert(ok::arch::detail::TerminalHoldBackend<ReturningBackend>);
static_assert(noexcept(ok::arch::terminal_power_action(ok::SystemPowerAction::halt)));
static_assert(noexcept(ok::arch::close_shutdown_interrupt_admission()));

struct Ports
{
    static inline unsigned reads{}, writes{}, pauses{}, busy_reads{}, interrupt_closures{}, final_holds{};
    static inline bool absent{}, order_ok{true};
    static ok::u8 read_status() noexcept
    {
        ++reads;
        return absent ? 0xff : reads <= busy_reads ? 2 : 0;
    }
    static void write_command(ok::u8 value) noexcept
    {
        order_ok &= value == 0xfe && interrupt_closures == 1 && !writes;
        ++writes;
    }
    static void pause() noexcept { ++pauses; }
    static void reset(unsigned busy = 0, bool missing = false) noexcept
    {
        reads = writes = pauses = interrupt_closures = final_holds = 0;
        busy_reads = busy; absent = missing; order_ok = true;
    }
};
struct ResetBackend
{
    static constexpr bool halt_supported = true, reboot_supported = true, poweroff_supported = false;
    static ok::Status preflight_poweroff() noexcept { return ok::Status::unsupported("disabled poweroff"); }
    static ok::Status request_poweroff() noexcept { return ok::Status::unsupported("disabled poweroff"); }
    static ok::Status preflight_reboot() noexcept { return ok::arch::detail::Pc8042Reset<Ports>::preflight(); }
    static ok::Status request_reboot() noexcept { return ok::arch::detail::Pc8042Reset<Ports>::request(); }
    static void disable_interrupts() noexcept { ++Ports::interrupt_closures; }
    static void hold_once() noexcept { std::longjmp(escape, 1); }
    static void hold_forever() noexcept
    {
        ++Ports::final_holds;
        Ports::order_ok &= Ports::interrupt_closures == 1;
        std::longjmp(escape, 1);
    }
};
struct DisabledResetBackend : ResetBackend { static constexpr bool reboot_supported = false; };
struct PoweroffBackend : ResetBackend
{
    static constexpr bool poweroff_supported = true;
    static inline unsigned probes{}, requests{};
    static inline bool available{true};
    static ok::Status preflight_poweroff() noexcept
    {
        ++probes;
        return available ? ok::Status{} : ok::Status::unsupported("selected PM device unavailable");
    }
    static ok::Status request_poweroff() noexcept
    {
        ++requests;
        Ports::order_ok &= Ports::interrupt_closures == 1;
        return preflight_poweroff();
    }
};
struct DisabledPoweroffBackend : PoweroffBackend { static constexpr bool poweroff_supported = false; };

bool poweroff_dispatch()
{
    using namespace ok;
    using namespace ok::arch::detail;
    for (const bool available : {false, true})
    {
        PoweroffBackend::available = available;
        PoweroffBackend::probes = PoweroffBackend::requests = 0;
        Ports::reset();
        auto status = terminal_preflight<PoweroffBackend>(SystemPowerAction::poweroff);
        if (status.ok() != available || PoweroffBackend::probes != 1 || PoweroffBackend::requests ||
            Ports::interrupt_closures) return false;
        // Final dispatch rechecks rather than trusting reservation-time state.
        PoweroffBackend::available = !available;
        if (setjmp(escape) == 0) terminal_dispatch<PoweroffBackend>(SystemPowerAction::poweroff);
        if (PoweroffBackend::probes != 2 || PoweroffBackend::requests != 1 ||
            Ports::final_holds != 1 || !Ports::order_ok) return false;
    }
    for (unsigned value = 0; value != 256; ++value)
    {
        const auto action = static_cast<SystemPowerAction>(value);
        if (action == SystemPowerAction::poweroff || action == SystemPowerAction::reboot) continue;
        PoweroffBackend::probes = PoweroffBackend::requests = 0;
        Ports::reset();
        static_cast<void>(terminal_preflight<PoweroffBackend>(action));
        if (setjmp(escape) == 0) terminal_dispatch<PoweroffBackend>(action);
        if (PoweroffBackend::probes || PoweroffBackend::requests || Ports::writes ||
            Ports::final_holds != 1 || !Ports::order_ok) return false;
    }
    PoweroffBackend::probes = PoweroffBackend::requests = 0;
    Ports::reset();
    if (terminal_supported<DisabledPoweroffBackend>(SystemPowerAction::poweroff) ||
        terminal_preflight<DisabledPoweroffBackend>(SystemPowerAction::poweroff).code() != StatusCode::unsupported)
        return false;
    if (setjmp(escape) == 0) terminal_dispatch<DisabledPoweroffBackend>(SystemPowerAction::poweroff);
    return !PoweroffBackend::probes && !PoweroffBackend::requests && Ports::final_holds == 1 && Ports::order_ok;
}


bool reset_policy()
{
    using namespace ok;
    using namespace ok::arch::detail;
    Ports::reset(3);
    if (!terminal_preflight<ResetBackend>(SystemPowerAction::reboot) || Ports::reads != 4 ||
        Ports::pauses != 3 || Ports::writes || Ports::interrupt_closures) return false;
    Ports::reset(Pc8042Reset<Ports>::poll_limit);
    if (terminal_preflight<ResetBackend>(SystemPowerAction::reboot).code() != StatusCode::busy ||
        Ports::reads != Pc8042Reset<Ports>::poll_limit || Ports::writes) return false;
    Ports::reset(0, true);
    if (terminal_preflight<ResetBackend>(SystemPowerAction::reboot).code() != StatusCode::unsupported ||
        Ports::reads != 1 || Ports::writes) return false;
    for (const auto action : {SystemPowerAction::halt, SystemPowerAction::poweroff, SystemPowerAction::none,
                              static_cast<SystemPowerAction>(255)})
    {
        Ports::reset();
        static_cast<void>(terminal_preflight<ResetBackend>(action));
        if (Ports::reads || Ports::writes) return false;
        if (setjmp(escape) == 0) terminal_dispatch<ResetBackend>(action);
        if (Ports::reads || Ports::writes || Ports::final_holds != 1 || !Ports::order_ok) return false;
    }
    Ports::reset();
    if (terminal_preflight<DisabledResetBackend>(SystemPowerAction::reboot).code() != StatusCode::unsupported ||
        Ports::reads) return false;
    if (setjmp(escape) == 0) terminal_dispatch<DisabledResetBackend>(SystemPowerAction::reboot);
    if (Ports::reads || Ports::writes || Ports::final_holds != 1 || !Ports::order_ok) return false;
    for (const unsigned busy : {0u, 3u, static_cast<unsigned>(Pc8042Reset<Ports>::poll_limit)})
    {
        Ports::reset(busy);
        if (setjmp(escape) == 0) terminal_dispatch<ResetBackend>(SystemPowerAction::reboot);
        if (!Ports::order_ok || Ports::final_holds != 1 || Ports::interrupt_closures != 1 ||
            Ports::writes != static_cast<unsigned>(busy < Pc8042Reset<Ports>::poll_limit)) return false;
    }
    // Readiness at reservation is no promise about the later terminal boundary.
    Ports::reset();
    if (!terminal_preflight<ResetBackend>(SystemPowerAction::reboot)) return false;
    Ports::busy_reads = Pc8042Reset<Ports>::poll_limit + Ports::reads;
    if (setjmp(escape) == 0) terminal_dispatch<ResetBackend>(SystemPowerAction::reboot);
    return Ports::writes == 0 && Ports::final_holds == 1 && Ports::order_ok;
}
} // namespace

int main()
{
    using ok::SystemPowerAction;
    constexpr bool supported = ok::arch::configured_architecture() == ok::arch::Architecture::x86_64;
    for (unsigned value = 0; value != 256; ++value)
    {
        const auto action = static_cast<SystemPowerAction>(value);
#if defined(OK_NATIVE_PC8042_REBOOT)
        constexpr bool reboot = supported;
#else
        constexpr bool reboot = false;
#endif
#if defined(OK_NATIVE_QEMU_PIIX4_POWEROFF)
        constexpr bool poweroff = supported;
#else
        constexpr bool poweroff = false;
#endif
        if (poweroff && action == SystemPowerAction::poweroff)
        {
            if (!ok::arch::terminal_power_action_supported(action)) return 1;
            continue;
        }
        // The enabled production preflight reads hardware. Its shared policy is
        // executed above with fake ports, never with host privileged I/O.
        if (reboot && action == SystemPowerAction::reboot)
        {
            if (!ok::arch::terminal_power_action_supported(action)) return 1;
            continue;
        }
        const auto status = ok::arch::preflight_terminal_power_action(action);
        const bool valid = action == SystemPowerAction::halt || action == SystemPowerAction::poweroff ||
                           action == SystemPowerAction::reboot;
        const bool accepted = supported && action == SystemPowerAction::halt;
        if (status.ok() != accepted || ok::arch::terminal_power_action_supported(action) != accepted ||
            (!valid && status.code() != ok::StatusCode::invalid_argument) ||
            (valid && !accepted && status.code() != ok::StatusCode::unsupported)) return 1;
    }
    // The qualified x86_64 closure contains cli and must not run in a host process.
    if constexpr (!supported)
        if (ok::arch::close_shutdown_interrupt_admission().code() != ok::StatusCode::unsupported) return 1;
    if (setjmp(escape) == 0) ok::arch::detail::terminal_hold<ReturningBackend>();
    if (!ordered || disables != 4 || holds != 4) return 1;
    if (!reset_policy() || !poweroff_dispatch()) return 1;
    std::cout << "TERMINAL_POWER_HOST_PASS profile=" << ok::arch::to_string(ok::arch::configured_architecture())
              << " values=256 returned-holds=4 pc8042=fake-ports scope=model-and-readonly-capability\n";
}
