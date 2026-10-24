#pragma once
#include "ok/abi/linux_socketpair.hpp"
#include "ok/abi/linux_tcp_data.hpp"
#include "ok/uapi/syscall.h"

namespace ok::abi::linux_user
{
enum class UnixPathCall : u8 { none, socket, bind, listen, connect, accept, accept4, getsockopt };
// Linux sockaddr_un has a two-byte target-endian family and 108 pathname bytes
// on every supported profile. Abstract and unterminated names are unsupported.
template <Profile Selected, bool Native = false> requires SupportedProfile<Selected>
struct UnixPathCodec final
{
    static constexpr bool big_endian = Codec<Selected>::profile.big_endian;
    static constexpr usize path_capacity = 108, address_capacity = 110;
    static_assert(address_capacity == sched::unix_pathname_address_capacity);
    static_assert(path_capacity - 1 == sched::unix_pathname_text_capacity);
    static constexpr u64 word_mask = Codec<Selected>::word_mask;
    static constexpr u32 nonblock = Native ? OK_SOCK_NONBLOCK : OpenCodec<Selected>::nonblock;
    static constexpr u32 cloexec = Native ? OK_SOCK_CLOEXEC : OpenCodec<Selected>::cloexec;
    static constexpr u32 stream = Native ? OK_SOCK_STREAM : SocketPairCodec<Selected>::stream;
    static constexpr u32 sol_socket = Native ? OK_SOL_SOCKET : tcp_sol_socket[static_cast<usize>(Selected)];
    static constexpr u32 peercred = Native ? OK_SO_PEERCRED : (Selected == Profile::mips || Selected == Profile::mips64 ? 18u : Selected == Profile::ppc ? 21u : 17u);
    static constexpr u32 so_type = Native ? OK_SO_TYPE : tcp_so_type[static_cast<usize>(Selected)];
    static constexpr u32 so_error = Native ? OK_SO_ERROR : tcp_so_error[static_cast<usize>(Selected)];
    static constexpr bool range(u64 address, usize bytes)
    { return address && bytes && address <= word_mask && bytes - 1 <= word_mask - address; }
    static constexpr u64 load(std::span<const u8> bytes)
    {
        u64 value{};
        for (usize i = 0; i < bytes.size(); ++i)
            value |= static_cast<u64>(bytes[i]) << (8 * (big_endian ? bytes.size() - 1 - i : i));
        return value;
    }
    static constexpr void store(std::span<u8> bytes, u64 value)
    {
        for (usize i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<u8>(value >> (8 * (big_endian ? bytes.size() - 1 - i : i)));
    }
    static Result<u32> decode_type(u64 type)
    {
        if (type > 0xffffffffu || (type & 0xf) != stream || (type & ~(u64{0xf} | nonblock | cloexec)))
            return Status::unsupported("bounded UNIX pathname sockets require stream type");
        return ((type & nonblock) ? posix::o_NONBLOCK : 0u) | ((type & cloexec) ? posix::o_CLOEXEC : 0u);
    }
    static Result<FixedString<108>> decode_address(std::span<const u8> bytes)
    {
        if (bytes.size() < 3 || bytes.size() > address_capacity || load(bytes.first(2)) != 1)
            return Status::invalid_argument("UNIX pathname address has another family or size");
        if (!bytes[2]) return Status::unsupported("abstract UNIX addresses are unsupported");
        FixedString<108> path;
        for (usize i = 2; i < bytes.size(); ++i)
        {
            if (!bytes[i]) return path;
            if (i - 2 >= path_capacity - 1 || !path.append(static_cast<char>(bytes[i])))
                return Status::name_too_long("UNIX pathname exceeds 107 bytes");
        }
        return Status::invalid_argument("UNIX pathname must include its terminator");
    }
};
}
