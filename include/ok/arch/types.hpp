#pragma once

#include "ok/core/types.hpp"

#include <array>
#include <string_view>

namespace ok::arch
{

enum class Architecture : u8
{
    i386,
    x86_64,
    aarch64,
    arm32,
    rv64,
    rv32,
    loongarch64,
    mips,
    mips64,
    ppc,
};

enum class Endianness : u8
{
    little,
    big,
};

enum class PrivilegeMode : u8
{
    kernel,
    user,
};

struct CpuContext
{
    Architecture architecture{Architecture::x86_64};
    std::array<uptr, 32> registers{};
    uptr program_counter{0};
    uptr stack_pointer{0};
    uptr status_register{0};
    PrivilegeMode mode{PrivilegeMode::kernel};
};

struct TrapFrame
{
    u64 vector{0};
    u64 error_code{0};
    CpuContext context{};
};

struct UserEntry
{
    uptr instruction_pointer{0};
    uptr stack_pointer{0};
    uptr argument{0};
};

} // namespace ok::arch
