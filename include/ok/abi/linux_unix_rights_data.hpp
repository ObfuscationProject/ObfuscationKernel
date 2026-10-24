#pragma once

// Generated from exact musl 1.2.5 archive; see docs/abi/linux-unix-rights-source.json.
#include "ok/abi/linux_user.hpp"

namespace ok::abi::linux_user
{
struct UnixMessageNumbers { u32 sendmsg, recvmsg, socketcall; };
inline constexpr std::array<UnixMessageNumbers, static_cast<usize>(Profile::count)> unix_message_numbers{{
    {370u, 372u, 102u}, // i386
    {46u, 47u, 0u}, // x86_64
    {211u, 212u, 0u}, // aarch64
    {296u, 297u, 0u}, // arm
    {211u, 212u, 0u}, // riscv64
    {211u, 212u, 0u}, // riscv32
    {211u, 212u, 0u}, // loongarch64
    {4179u, 4177u, 4102u}, // mips
    {5045u, 5046u, 0u}, // mips64
    {341u, 342u, 102u}, // powerpc
}};
} // namespace ok::abi::linux_user
