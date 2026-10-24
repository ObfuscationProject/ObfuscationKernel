// Hosted seam for actual runtime dispatch. No privileged instructions execute.
#include "native_power_backend_model.hpp"
#include <cstdlib>

namespace ok::arch
{
bool terminal_power_action_supported(SystemPowerAction action) noexcept
{
    return action == SystemPowerAction::halt ||
           (action == SystemPowerAction::reboot && hosted_power::reboot_enabled);
}
Status preflight_terminal_power_action(SystemPowerAction action) noexcept
{
    ++hosted_power::preflights;
    if (action != SystemPowerAction::halt && action != SystemPowerAction::reboot && action != SystemPowerAction::poweroff)
        return Status::invalid_argument("hosted invalid action");
    if (!terminal_power_action_supported(action)) return Status::unsupported("hosted backend unavailable");
    return action == SystemPowerAction::reboot ? hosted_power::reboot_status : Status{};
}
Status close_shutdown_interrupt_admission() noexcept
{
    ++hosted_power::interrupt_closures;
    return {};
}
[[noreturn]] void terminal_power_action(SystemPowerAction) noexcept { std::abort(); }
} // namespace ok::arch
