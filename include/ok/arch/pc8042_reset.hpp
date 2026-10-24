#pragma once

#include "ok/core/types.hpp"
#include <concepts>

namespace ok::arch::detail
{
template <typename Ports>
concept Pc8042Ports = requires {
    { Ports::read_status() } noexcept -> std::same_as<u8>;
    { Ports::write_command(u8{}) } noexcept -> std::same_as<void>;
    { Ports::pause() } noexcept -> std::same_as<void>;
};

// A selected PC profile supplies machine admission; readiness is not discovery.
template <Pc8042Ports Ports> struct Pc8042Reset final
{
    static constexpr usize poll_limit = 100000;
    static Status preflight() noexcept
    {
        for (usize attempt = 0; attempt < poll_limit; ++attempt)
        {
            const auto status = Ports::read_status();
            if (status == 0xff) return Status::unsupported("PC/8042 controller is unavailable");
            if ((status & 2u) == 0) return {};
            Ports::pause();
        }
        return Status::busy("PC/8042 controller input remained busy");
    }
    static Status request() noexcept
    {
        if (auto status = preflight(); !status) return status;
        Ports::write_command(0xfe);
        return {};
    }
};
} // namespace ok::arch::detail
