#pragma once

#include "ok/core/types.hpp"

namespace ok::arch
{

// Coordinates only. A range is not proof of mapping permissions, faulting
// guards, CPU ownership, lifetime or a complete stack-usage bound.
struct StackBounds final
{
    uptr lower{}, top{};

    [[nodiscard]] constexpr bool valid() const noexcept { return lower != 0 && top > lower; }
    [[nodiscard]] constexpr bool contains(uptr sp) const noexcept
    {
        // top is an admissible initial SP on these downward-growing stacks.
        return valid() && sp >= lower && sp <= top;
    }
    [[nodiscard]] Result<usize> remaining(uptr sp) const noexcept
    {
        if (!valid()) return Status::invalid_argument("stack endpoints are not ordered");
        if (!contains(sp)) return Status::fault("stack pointer is outside its expected stack");
        return static_cast<usize>(sp - lower);
    }
    [[nodiscard]] Status require(uptr sp, usize suffix_bytes) const noexcept
    {
        const auto bytes = remaining(sp);
        if (!bytes) return bytes.status();
        // Compare remaining bytes, rather than adding suffix to an address.
        if (suffix_bytes > bytes.value()) return Status::no_memory("stack suffix exceeds remaining bytes");
        return {};
    }
};

} // namespace ok::arch
