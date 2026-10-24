#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/power.hpp"

namespace ok::arch
{

// Selected backend capability; callers separately prove retirement and clean sealing.
[[nodiscard]] bool terminal_power_action_supported(SystemPowerAction action) noexcept;
[[nodiscard]] Status preflight_terminal_power_action(SystemPowerAction action) noexcept;
[[nodiscard]] Status close_shutdown_interrupt_admission() noexcept;

// Kernel-internal final boundary. Unsupported values also remain here forever.
[[noreturn]] void terminal_power_action(SystemPowerAction action) noexcept;

namespace detail
{

template <typename Backend>
concept TerminalHoldBackend = requires {
    { Backend::disable_interrupts() } noexcept -> std::same_as<void>;
    { Backend::hold_once() } noexcept -> std::same_as<void>;
};

template <TerminalHoldBackend Backend> [[noreturn]] void terminal_hold() noexcept
{
    for (;;)
    {
        Backend::disable_interrupts();
        Backend::hold_once();
    }
}

template <typename Backend>
concept TerminalPowerBackend = TerminalHoldBackend<Backend> && requires {
    { Backend::halt_supported } -> std::convertible_to<bool>;
    { Backend::reboot_supported } -> std::convertible_to<bool>;
    { Backend::poweroff_supported } -> std::convertible_to<bool>;
    { Backend::preflight_reboot() } noexcept -> std::same_as<Status>;
    { Backend::request_reboot() } noexcept -> std::same_as<Status>;
    { Backend::preflight_poweroff() } noexcept -> std::same_as<Status>;
    { Backend::request_poweroff() } noexcept -> std::same_as<Status>;
    { Backend::hold_forever() } noexcept -> std::same_as<void>;
};

template <TerminalPowerBackend Backend> bool terminal_supported(SystemPowerAction action) noexcept
{
    return (action == SystemPowerAction::halt && Backend::halt_supported) ||
           (action == SystemPowerAction::reboot && Backend::reboot_supported) ||
           (action == SystemPowerAction::poweroff && Backend::poweroff_supported);
}

template <TerminalPowerBackend Backend> Status terminal_preflight(SystemPowerAction action) noexcept
{
    if (action != SystemPowerAction::halt && action != SystemPowerAction::poweroff &&
        action != SystemPowerAction::reboot)
        return Status::invalid_argument("invalid terminal power action");
    if (!terminal_supported<Backend>(action))
        return Status::unsupported("terminal power action has no selected backend");
    if constexpr (Backend::reboot_supported)
        if (action == SystemPowerAction::reboot) return Backend::preflight_reboot();
    if constexpr (Backend::poweroff_supported)
        if (action == SystemPowerAction::poweroff) return Backend::preflight_poweroff();
    return {};
}

template <TerminalPowerBackend Backend> [[noreturn]] void terminal_dispatch(SystemPowerAction action) noexcept
{
    Backend::disable_interrupts();
    if constexpr (Backend::reboot_supported)
        if (action == SystemPowerAction::reboot) static_cast<void>(Backend::request_reboot());
    if constexpr (Backend::poweroff_supported)
        if (action == SystemPowerAction::poweroff) static_cast<void>(Backend::request_poweroff());
    Backend::hold_forever();
    // A misbehaving backend cannot resume ordinary kernel work.
    terminal_hold<Backend>();
}

} // namespace detail
} // namespace ok::arch
