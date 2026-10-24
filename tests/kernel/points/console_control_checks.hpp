#pragma once

#include "ok/user/console_control_dispatch.hpp"

namespace ok::tests
{
// Production scalar/copy marshalling under injected model seams only.
// This does not establish terminal authority, a trap or user execution.
template <bool BigEndian> Status check_console_control_wire_order()
{
    using Operation = sched::ConsoleControlOperation;
    std::array<u8, 4> output{};
    i32 selected{};
    usize controls{}, inputs{}, outputs{};
    const auto call = [&](i32 fd, std::optional<Operation> operation, u64 argument) {
        return user::console_control::dispatch<BigEndian>(fd, operation, argument,
            [](i32 descriptor) { return descriptor == 0 ? Status{} : Status::bad_descriptor(); },
            [&](i32, Operation request, i32 value) -> Result<i64> {
                ++controls;
                if (request == Operation::set_foreground) selected = value;
                return request == Operation::get_session || request == Operation::get_foreground
                    ? i64{0x12345678} : i64{0};
            },
            [&](u64 address, std::span<u8> bytes) {
                ++inputs;
                if (address != 4 || bytes.size() != 4) return Status::fault();
                const std::array<u8, 4> minus_two = BigEndian ? std::array<u8, 4>{255, 255, 255, 254}
                                                             : std::array<u8, 4>{254, 255, 255, 255};
                for (usize index = 0; index < 4; ++index) bytes[index] = minus_two[index];
                return Status{};
            },
            [](u64 address, usize bytes) { return address == 4 && bytes == 4; },
            [&](u64 address, std::span<const u8> bytes) {
                ++outputs;
                if (address != 4 || bytes.size() != 4) return Status::fault();
                for (usize index = 0; index < 4; ++index) output[index] = bytes[index];
                return Status{};
            });
    };
    if (call(1, {}, 0) != -9 || call(0, {}, 0) != -25 ||
        call(0, Operation::acquire, 1) != -95 || controls || inputs || outputs)
        return Status::fault("console control descriptor/request/scalar ordering failed");
    if (call(0, Operation::acquire, 0) != 0 || controls != 1 || inputs || outputs ||
        call(0, Operation::set_foreground, 4) != 0 || selected != -2 || inputs != 1 ||
        call(0, Operation::get_foreground, 4) != 0 || outputs != 1)
        return Status::fault("console control exact PID model failed");
    const std::array<u8, 4> expected = BigEndian ? std::array<u8, 4>{0x12, 0x34, 0x56, 0x78}
                                                : std::array<u8, 4>{0x78, 0x56, 0x34, 0x12};
    if (output != expected || call(0, Operation::get_session, 0) != -14 || outputs != 1 ||
        call(0, Operation::set_foreground, 0) != -14 || selected != -2)
        return Status::fault("console control byte order or refusal model failed");
    return {};
}

inline Status check_console_control_wire_model()
{
    if (auto status = check_console_control_wire_order<false>(); !status) return status;
    return check_console_control_wire_order<true>();
}
} // namespace ok::tests
