#pragma once

#include "ok/user/console_control_dispatch.hpp"

namespace ok::tests
{
// Production scalar/copy marshalling under injected model seams only.
// This does not establish terminal authority, a trap or user execution.
template <bool BigEndian> Status check_console_control_wire_order()
{
    using Operation = sched::ConsoleControlOperation;
    using Codec = user::console_control::NativeTerminalCodec<BigEndian>;
    std::array<u8, 36> output{};
    i32 selected{};
    tty::Termios stored{};
    usize checks{}, controls{}, inputs{}, outputs{};
    bool refuse{};
    const auto call = [&](i32 fd, std::optional<Operation> operation, u64 argument) {
        return user::console_control::dispatch<Codec>(fd, operation, argument,
            [](i32 descriptor) -> Result<sched::TerminalSubject> {
                if (descriptor) return Status::bad_descriptor();
                return sched::TerminalSubject{0, sched::console_terminal_ref, sched::TerminalSide::console};
            },
            [&](const sched::TerminalSubject &, Operation) {
                ++checks;
                return refuse ? Status::not_terminal() : Status{};
            },
            [&](u64 address, std::span<u8> bytes) {
                ++inputs;
                if (address != 4 || (bytes.size() != 4 && bytes.size() != 36)) return Status::fault();
                const std::array<u8, 4> minus_two = BigEndian ? std::array<u8, 4>{255, 255, 255, 254}
                                                             : std::array<u8, 4>{254, 255, 255, 255};
                for (usize index = 0; index < bytes.size(); ++index)
                    bytes[index] = index < 4 ? minus_two[index] : static_cast<u8>(index);
                return Status{};
            },
            [&](const sched::TerminalSubject &, const sched::TerminalRequest &request) -> Result<sched::TerminalReply> {
                ++controls;
                if (request.operation == Operation::set_foreground || request.operation == Operation::acquire)
                    selected = request.scalar;
                if (request.operation == Operation::set_now) stored = request.termios;
                sched::TerminalReply reply{};
                reply.value = request.operation == Operation::get_session || request.operation == Operation::get_foreground
                    ? i64{0x12345678} : i64{0};
                reply.termios = stored;
                return reply;
            },
            [](u64 address, usize bytes) { return address == 4 && (bytes == 4 || bytes == 36); },
            [&](u64 address, std::span<const u8> bytes) {
                ++outputs;
                if (address != 4 || bytes.size() > output.size()) return Status::fault();
                for (usize index = 0; index < bytes.size(); ++index) output[index] = bytes[index];
                return Status{};
            });
    };
    // Descriptor, request and association all precede any copy.
    refuse = true;
    if (call(1, {}, 0) != -9 || call(0, {}, 0) != -25 || call(0, Operation::set_foreground, 4) != -25 ||
        checks != 1 || controls || inputs || outputs)
        return Status::fault("console control descriptor/request/association ordering failed");
    refuse = false;
    // A forced TIOCSCTTY reaches the control with its scalar (Linux tiocsctty).
    if (call(0, Operation::acquire, 1) != 0 || selected != 1 || controls != 1 || inputs || outputs ||
        call(0, Operation::set_foreground, 4) != 0 || selected != -2 || inputs != 1 ||
        call(0, Operation::get_foreground, 4) != 0 || outputs != 1)
        return Status::fault("console control exact PID model failed");
    const std::array<u8, 4> expected = BigEndian ? std::array<u8, 4>{0x12, 0x34, 0x56, 0x78}
                                                : std::array<u8, 4>{0x78, 0x56, 0x34, 0x12};
    if (output[0] != expected[0] || output[1] != expected[1] || output[2] != expected[2] ||
        output[3] != expected[3] || call(0, Operation::get_session, 0) != -14 || outputs != 1 ||
        call(0, Operation::set_foreground, 0) != -14 || selected != -2)
        return Status::fault("console control byte order or refusal model failed");
    // struct ok_termios: exactly 36 bytes each way, target-endian flag words.
    if (call(0, Operation::set_now, 4) != 0 || inputs != 3 || call(0, Operation::get_attributes, 4) != 0 ||
        outputs != 2 || stored.iflag != std::bit_cast<u32>(i32{-2}) || stored.cc[0] != 17 || output[35] != 35 ||
        output[0] != (BigEndian ? 255 : 254) || call(0, Operation::get_attributes, 0) != -14 || outputs != 2)
        return Status::fault("console control native termios wire model failed");
    return {};
}

inline Status check_console_control_wire_model()
{
    if (auto status = check_console_control_wire_order<false>(); !status) return status;
    return check_console_control_wire_order<true>();
}
} // namespace ok::tests
