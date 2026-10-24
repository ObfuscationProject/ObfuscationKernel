#pragma once
// Hosted link support for an unexecuted native-personality branch in Linux models.
// This is the exact x86_64 production syscall decoder; it grants no capture authority.
#include "ok/arch/execution.hpp"

namespace ok::arch::execution
{
Syscall syscall(const UserFrame &frame)
{
    return {frame.registers[14],
            {frame.registers[9], frame.registers[10], frame.registers[11], frame.registers[5],
             frame.registers[7], frame.registers[6]}};
}
}
