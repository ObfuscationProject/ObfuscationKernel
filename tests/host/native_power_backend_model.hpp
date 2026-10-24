#pragma once
#include "ok/arch/terminal_power.hpp"

namespace ok::arch::hosted_power
{
inline bool reboot_enabled{};
inline Status reboot_status{};
inline usize preflights{}, interrupt_closures{};
} // namespace ok::arch::hosted_power
