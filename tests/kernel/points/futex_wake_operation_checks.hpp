#pragma once

#include "ok/abi/linux_futex_wake_operation.hpp"
#include <array>

namespace ok::tests
{
// Algorithm coverage only; actual user memory and waiter queues need native tests.
inline Status check_futex_wake_operation_algorithm()
{
    using Operation = abi::linux_user::FutexWakeOperation;
    const auto encode = [](u32 operation, u32 comparison, i32 operand, i32 compared, bool shift = false)
    {
        return (operation << 28) | (comparison << 24) |
               ((static_cast<u32>(operand) & 0xfffU) << 12) |
               (static_cast<u32>(compared) & 0xfffU) | (shift ? 0x80000000U : 0U);
    };
    struct Modification
    {
        u32 operation, previous;
        i32 operand;
        bool shift;
        u32 expected;
    };
    const std::array modifications{
        Modification{0, 7, -2048, false, 0xfffff800U},
        Modification{0, 7, 2047, false, 2047},
        Modification{1, 0xffffffffU, 1, false, 0},
        Modification{1, 0, -1, false, 0xffffffffU},
        Modification{2, 0x50, 3, false, 0x53},
        Modification{3, 0xff, 0x0f, false, 0xf0},
        Modification{4, 0xaa, 0x0f, false, 0xa5},
        Modification{0, 7, -1, true, 0x80000000U},
        Modification{0, 7, 32, true, 1},
        Modification{1, 0xffffffffU, 33, true, 1},
    };
    for (const auto &fixture : modifications)
    {
        const auto operation = Operation::decode(encode(fixture.operation, 0, fixture.operand, -2048, fixture.shift));
        const auto value = operation.modify(fixture.previous);
        if (operation.operation != fixture.operation || operation.operand != fixture.operand ||
            operation.compare_operand != -2048 || operation.shifted != fixture.shift ||
            !value || value.value() != fixture.expected)
            return Status::fault("futex wake operation arithmetic or signed operand mismatch");
    }
    struct Comparison
    {
        u32 operation, previous;
        i32 operand;
        bool expected;
    };
    const std::array comparisons{
        Comparison{0, 0xffffffffU, -1, true}, Comparison{0, 0, -1, false},
        Comparison{1, 0, -1, true}, Comparison{1, 0xffffffffU, -1, false},
        Comparison{2, 0x80000000U, -2048, true}, Comparison{2, 0, -1, false},
        Comparison{3, 0xffffffffU, -1, true}, Comparison{3, 1, 0, false},
        Comparison{4, 0x7fffffffU, 2047, true}, Comparison{4, 0xffffffffU, 0, false},
        Comparison{5, 0xffffffffU, -1, true}, Comparison{5, 0x80000000U, -2048, false},
    };
    for (const auto &fixture : comparisons)
    {
        const auto operation = Operation::decode(encode(0, fixture.operation, 0, fixture.operand));
        const auto value = operation.compare(fixture.previous);
        if (!value || value.value() != fixture.expected)
            return Status::fault("futex wake operation signed comparison mismatch");
    }
    for (u32 operation = 5; operation < 8; ++operation)
        if (Operation::decode(encode(operation, 0, 1, 0)).modify(7))
            return Status::fault("undefined futex arithmetic was accepted");
    for (u32 comparison = 6; comparison < 16; ++comparison)
        if (Operation::decode(encode(0, comparison, 1, 0)).compare(7))
            return Status::fault("undefined futex comparison was accepted");
    return Status::success();
}
} // namespace ok::tests
