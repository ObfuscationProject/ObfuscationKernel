#pragma once

#include "ok/sched/console_control.hpp"
#include "ok/syscall/linux.hpp"
#include "ok/uapi/syscall.h"
#include <array>
#include <bit>
#include <optional>
#include <span>

namespace ok::user::console_control
{
using Operation = sched::ConsoleControlOperation;

[[nodiscard]] inline std::optional<Operation> native_operation(u64 request)
{
    switch (request)
    {
    case OK_TIOCSCTTY: return Operation::acquire;
    case OK_TIOCGSID: return Operation::get_session;
    case OK_TIOCGPGRP: return Operation::get_foreground;
    case OK_TIOCSPGRP: return Operation::set_foreground;
    default: return {};
    }
}

// Callers bind the running canonical process before entering these seams.
// Every pointed PID is four target-endian bytes, including LP64 profiles.
template <bool BigEndian, typename Describe, typename Control, typename CopyIn,
          typename Writable, typename CopyOut>
i64 dispatch(i32 fd, std::optional<Operation> operation, u64 argument,
             Describe &&describe, Control &&control, CopyIn &&copy_in,
             Writable &&writable, CopyOut &&copy_out)
{
    if (const auto status = describe(fd); !status)
        return -syscall::ErrnoMapper::errno_for(status);
    if (!operation) return -25;
    i32 value{};
    if (*operation == Operation::acquire)
    {
        if (argument) return -95;
    }
    else if (*operation == Operation::set_foreground)
    {
        std::array<u8, 4> input{};
        if (!copy_in(argument, std::span<u8>{input})) return -14;
        u32 raw{};
        for (usize index = 0; index < input.size(); ++index)
            raw |= static_cast<u32>(input[index]) << (8 * (BigEndian ? 3 - index : index));
        value = std::bit_cast<i32>(raw);
    }
    const auto result = control(fd, *operation, value);
    if (!result) return -syscall::ErrnoMapper::errno_for(result.status());
    if (*operation == Operation::acquire || *operation == Operation::set_foreground)
        return result.value();
    if (result.value() < 0 || result.value() > 0x7fffffff) return -75;
    if (!writable(argument, usize{4})) return -14;
    const auto raw = static_cast<u32>(result.value());
    std::array<u8, 4> output{};
    for (usize index = 0; index < output.size(); ++index)
        output[index] = static_cast<u8>(raw >> (8 * (BigEndian ? 3 - index : index)));
    return copy_out(argument, std::span<const u8>{output}) ? i64{0} : i64{-14};
}
} // namespace ok::user::console_control
