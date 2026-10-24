#pragma once

#include "ok/core/types.hpp"
#include <bit>

namespace ok::abi::linux_user
{
// Linux UAPI FUTEX_OP words have signed twelve-bit operands. Evaluation is
// independent of host endianness and never overflows a signed C++ expression.
struct FutexWakeOperation
{
    u32 operation{}, comparison{};
    i32 operand{}, compare_operand{};
    bool shifted{};

    [[nodiscard]] static constexpr i32 signed_operand(u32 value)
    {
        return static_cast<i32>(value & 0x7ffU) - static_cast<i32>(value & 0x800U);
    }
    [[nodiscard]] static constexpr FutexWakeOperation decode(u32 encoded)
    {
        return {(encoded >> 28) & 7U, (encoded >> 24) & 15U,
            signed_operand((encoded >> 12) & 0xfffU), signed_operand(encoded & 0xfffU),
            (encoded & 0x80000000U) != 0};
    }
    [[nodiscard]] Result<u32> modify(u32 previous) const
    {
        const u32 value = shifted ? u32{1} << (static_cast<u32>(operand) & 31U)
                                  : static_cast<u32>(operand);
        switch (operation)
        {
        case 0: return value;
        case 1: return previous + value;
        case 2: return previous | value;
        case 3: return previous & ~value;
        case 4: return previous ^ value;
        default: return Status::unsupported("Linux futex wake operation is not defined");
        }
    }
    [[nodiscard]] Result<bool> compare(u32 previous) const
    {
        const i32 value = std::bit_cast<i32>(previous);
        switch (comparison)
        {
        case 0: return value == compare_operand;
        case 1: return value != compare_operand;
        case 2: return value < compare_operand;
        case 3: return value <= compare_operand;
        case 4: return value > compare_operand;
        case 5: return value >= compare_operand;
        default: return Status::unsupported("Linux futex wake comparison is not defined");
        }
    }
};
} // namespace ok::abi::linux_user
