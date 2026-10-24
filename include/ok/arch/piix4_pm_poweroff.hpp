#pragma once

#include "ok/core/types.hpp"
#include <concepts>

namespace ok::arch::detail
{
template <typename Ports>
concept Piix4ConfigurationPorts = requires {
    { Ports::read_configuration(u8{}, u8{}, u8{}) } noexcept -> std::same_as<u32>;
};

template <typename Ports>
concept Piix4PmControlPorts = requires {
    { Ports::read_control(u16{}) } noexcept -> std::same_as<u16>;
    { Ports::write_control(u16{}, u16{}) } noexcept -> std::same_as<void>;
};

// This is an observation of selected bus-0 configuration, not a reservation.
struct Piix4PmDescriptor final
{
    u8 slot{};
    u8 function{};
    u8 header{};
    u8 function_zero_header{};
    u32 function_zero_identity{};
    u32 pm_base_register{};
    u8 decode_register{};
    u16 pm_base{};
    u16 control_port{};

    constexpr bool operator==(const Piix4PmDescriptor &) const noexcept = default;
};

// Admission is deliberately limited to the selected QEMU PIIX4 PM profile.
// The caller must exclude configuration/resource changes and concurrent users.
template <Piix4ConfigurationPorts Configuration, Piix4PmControlPorts Control>
struct Piix4PmPoweroff final
{
    static constexpr u32 identity = 0x71138086u;
    static constexpr u32 class_revision = 0x06800003u;
    static constexpr u16 minimum_pm_base = 0x0400u;
    static constexpr u16 maximum_pm_base = 0xffc0u;
    static constexpr usize maximum_probe_configuration_reads = 522;

    static Result<Piix4PmDescriptor> probe() noexcept
    {
        Piix4PmDescriptor selected{};
        bool found = false;
        for (u8 slot = 0; slot < 32; ++slot)
        {
            const auto zero_identity = Configuration::read_configuration(slot, 0, 0x00);
            if (absent(zero_identity)) continue;
            if (!valid_identity(zero_identity))
                return Status::unsupported("Malformed PCI function-zero identity");
            const auto zero_header = header(Configuration::read_configuration(slot, 0, 0x0c));
            if (!valid_header(zero_header))
                return Status::unsupported("Reserved PCI function-zero header type");
            const u8 function_count = (zero_header & 0x80u) != 0 ? 8 : 1;
            for (u8 function = 0; function < function_count; ++function)
            {
                const auto function_identity = function == 0
                                                   ? zero_identity
                                                   : Configuration::read_configuration(slot, function, 0x00);
                if (absent(function_identity)) continue;
                if (!valid_identity(function_identity))
                    return Status::unsupported("Malformed PCI function identity");
                const auto function_header = function == 0
                                                 ? zero_header
                                                 : header(Configuration::read_configuration(slot, function, 0x0c));
                if (!valid_header(function_header))
                    return Status::unsupported("Reserved PCI header type");
                if (function_identity != identity) continue;
                if (found) return Status::unsupported("Ambiguous PIIX4 PM functions");
                if ((function_header & 0x7fu) != 0)
                    return Status::unsupported("PIIX4 PM requires a normal PCI header");
                if (Configuration::read_configuration(slot, function, 0x08) != class_revision)
                    return Status::unsupported("PIIX4 PM class or revision is unsupported");
                const auto base_register = Configuration::read_configuration(slot, function, 0x40);
                const auto decode_register = static_cast<u8>(Configuration::read_configuration(slot, function, 0x80));
                auto admitted = admit(slot, function, function_header, zero_header, zero_identity,
                                      base_register, decode_register);
                if (!admitted) return admitted.status();
                selected = admitted.value();
                found = true;
            }
        }
        if (!found) return Status::unsupported("Selected PIIX4 PM function is absent");
        if (!still_selected(selected))
            return Status::busy("PIIX4 PM configuration changed during discovery");
        return selected;
    }

    // Configuration-address transport writes are allowed; PM/configuration-data
    // writes and reads of the PM control register are not part of preflight.
    static Status preflight() noexcept
    {
        auto selected = probe();
        return selected ? Status::success() : selected.status();
    }

    static Status request() noexcept
    {
        auto first = probe();
        if (!first) return first.status();
        const auto initial_control = Control::read_control(first.value().control_port);
        if (auto status = valid_control(initial_control); !status) return status;

        // Re-enumerate the complete selected bus, including duplicate detection.
        auto final = probe();
        if (!final) return final.status();
        if (first.value() != final.value())
            return Status::busy("PIIX4 PM configuration changed before poweroff");
        const auto control = Control::read_control(final.value().control_port);
        if (auto status = valid_control(control); !status) return status;
        if (control != initial_control)
            return Status::busy("PIIX4 PM control changed before poweroff");

        // QEMU PIIX4 S5 is sleep type 0. Preserve every unrelated control bit.
        const auto shutdown = static_cast<u16>((control & ~0x3c00u) | 0x2000u);
        Control::write_control(final.value().control_port, shutdown);
        return {};
    }

  private:
    static constexpr bool absent(u32 value) noexcept
    {
        return (value & 0xffffu) == 0xffffu;
    }

    static constexpr bool valid_identity(u32 value) noexcept
    {
        return (value & 0xffffu) != 0 && (value >> 16) != 0xffffu;
    }

    static constexpr u8 header(u32 value) noexcept
    {
        return static_cast<u8>(value >> 16);
    }

    static constexpr bool valid_header(u8 value) noexcept
    {
        return (value & 0x7fu) <= 2;
    }

    static Result<Piix4PmDescriptor> admit(u8 slot, u8 function, u8 function_header,
                                          u8 zero_header, u32 zero_identity,
                                          u32 base_register, u8 decode_register) noexcept
    {
        if ((base_register & 1u) == 0 || (base_register & ~0x0000ffc1u) != 0)
            return Status::unsupported("PIIX4 PMBASE encoding is unsupported");
        const auto base = base_register & 0x0000ffc0u;
        if (base < minimum_pm_base || base > maximum_pm_base || base + 63u > 0xffffu)
            return Status::unsupported("PIIX4 PM block is outside the selected I/O window");
        // The complete 64-byte block must not overlap mechanism-1 CF8/CFC.
        if (base <= 0x0cffu && base + 63u >= 0x0cf8u)
            return Status::unsupported("PIIX4 PM block overlaps PCI configuration ports");
        if (decode_register != 0x01u)
            return Status::unsupported("PIIX4 PM decode is not exclusively enabled");
        return Piix4PmDescriptor{slot, function, function_header, zero_header, zero_identity,
                                 base_register, decode_register, static_cast<u16>(base),
                                 static_cast<u16>(base + 4u)};
    }

    static bool still_selected(const Piix4PmDescriptor &selected) noexcept
    {
        const auto zero_identity = Configuration::read_configuration(selected.slot, 0, 0x00);
        const auto zero_header = header(Configuration::read_configuration(selected.slot, 0, 0x0c));
        const auto selected_identity = Configuration::read_configuration(selected.slot, selected.function, 0x00);
        const auto selected_header = header(Configuration::read_configuration(selected.slot, selected.function, 0x0c));
        const auto selected_class = Configuration::read_configuration(selected.slot, selected.function, 0x08);
        const auto selected_base = Configuration::read_configuration(selected.slot, selected.function, 0x40);
        const auto selected_decode = static_cast<u8>(Configuration::read_configuration(selected.slot, selected.function, 0x80));
        return zero_identity == selected.function_zero_identity && zero_header == selected.function_zero_header &&
               selected_identity == identity && selected_header == selected.header && selected_class == class_revision &&
               selected_base == selected.pm_base_register && selected_decode == selected.decode_register;
    }

    static Status valid_control(u16 value) noexcept
    {
        if (value == 0xffffu) return Status::io_error("PIIX4 PM control reads as unavailable");
        if ((value & 0x2000u) != 0)
            return Status::busy("PIIX4 PM sleep enable is already set");
        return {};
    }
};
} // namespace ok::arch::detail
