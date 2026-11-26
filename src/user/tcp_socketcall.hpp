#pragma once
#include <ok/abi/linux_tcp_data.hpp>
#include <ok/abi/linux_udp_call.hpp>
#include <array>
#include <concepts>
#include <cstddef>
#include <span>

namespace ok::user::native
{
// Legacy socketcall(2) for TCP streams on the profiles that have it (i386,
// MIPS O32, 32-bit PowerPC). The selector and the argument array pointer are
// saved registers; the arguments are `nargs` target words in user memory
// (Linux net/socket.c). Accept and accept4 (5, 18) keep their copied accept
// route (TcpAcceptCall); socketpair, sendmsg and recvmsg (8, 16, 17) are UNIX
// calls; recvmmsg and sendmmsg are not served. Pure value helpers: callers
// copy the words themselves.
template <abi::linux_user::Profile Selected> requires abi::linux_user::SupportedProfile<Selected>
struct TcpSocketcall final
{
    using Target = abi::linux_user::Codec<Selected>;
    using Call = abi::linux_user::TcpCall;
    struct Route
    {
        Call call{Call::none};
        u32 selector{};
        u64 array{};
        usize words{};
        [[nodiscard]] usize bytes() const { return words * 4; }
    };
    // What the descriptor named by the first argument is.
    enum class Descriptor : u8 { other, tcp, udp };

    static constexpr bool available =
        abi::linux_user::tcp_socketcall_numbers[static_cast<usize>(Selected)] != abi::linux_user::unavailable_number &&
        Target::profile.word_bytes == 4;
    // Argument words per selector (net/socket.c nargs[]).
    static constexpr std::array<u8, 21> nargs{0, 3, 3, 3, 2, 3, 3, 3, 4, 4, 4, 6, 6, 2, 5, 5, 3, 3, 4, 5, 4};

    // The direct call a selector stands for on a stream. send and recv are
    // sendto and recvfrom without an address.
    [[nodiscard]] static constexpr Call call(u64 selector)
    {
        switch (selector)
        {
        case 1: return Call::socket;
        case 2: return Call::bind;
        case 3: return Call::connect;
        case 4: return Call::listen;
        case 6: return Call::getsockname;
        case 7: return Call::getpeername;
        case 9: case 11: return Call::sendto;
        case 10: case 12: return Call::recvfrom;
        case 13: return Call::shutdown;
        case 14: return Call::setsockopt;
        case 15: return Call::getsockopt;
        default: return Call::none;
        }
    }
    [[nodiscard]] static Route route(const abi::linux_user::RegisterFile &saved)
    {
        if constexpr (!available) return {};
        else
        {
            const u64 number = saved.integer[Target::profile.number_register];
            if (number != abi::linux_user::tcp_socketcall_numbers[static_cast<usize>(Selected)]) return {};
            const u64 selector = saved.integer[Target::profile.argument_registers[0]];
            const u64 array = saved.integer[Target::profile.argument_registers[1]];
            if (selector > Target::word_mask || array > Target::word_mask) return {};
            const Call selected = call(selector);
            if (selected == Call::none) return {};
            return {selected, static_cast<u32>(selector), array, nargs[selector]};
        }
    }
    // The argument array must be non-null and must not wrap.
    [[nodiscard]] static Status interval(const Route &route)
    {
        if (route.call == Call::none || !route.words) return Status::invalid_argument("not a TCP socketcall route");
        return !route.array || route.array > Target::word_mask - (route.bytes() - 1)
            ? Status::fault("TCP socketcall argument array is null or wraps") : Status{};
    }
    // One target word in the profile's byte order.
    [[nodiscard]] static u64 word(std::span<const std::byte> bytes)
    {
        u32 value{};
        for (usize index = 0; index < 4; ++index)
        {
            const usize shift = Target::profile.big_endian ? (3 - index) * 8 : index * 8;
            value |= static_cast<u32>(std::to_integer<u8>(bytes[index])) << shift;
        }
        return value;
    }
    // The copied argument words as direct-call arguments; send and recv
    // leave the address arguments 0.
    [[nodiscard]] static Result<std::array<u64, 6>> arguments(const Route &route, std::span<const std::byte> copied)
    {
        if (auto status = interval(route); !status) return status;
        if (copied.size() != route.bytes()) return Status::invalid_argument("TCP socketcall arguments have another size");
        std::array<u64, 6> out{};
        for (usize index = 0; index < route.words; ++index) out[index] = word(copied.subspan(index * 4, 4));
        return out;
    }
    // Whether TCP owns the call, from the first two argument words. socket
    // is TCP for SOCK_STREAM; getsockopt also serves UDP sockets (SO_ERROR,
    // SO_TYPE), as the direct call does; every other call needs a TCP
    // descriptor and is otherwise left to the UDP and UNIX paths.
    template <class Kind>
        requires requires(Kind kind, u64 fd) { { kind(fd) } -> std::same_as<Descriptor>; }
    [[nodiscard]] static Call owner(const Route &route, u64 first, u64 second, Kind &&kind)
    {
        using Constants = abi::linux_user::UdpConstants<Selected>;
        if (route.call == Call::none) return Call::none;
        if (route.call == Call::socket)
            return (second & Constants::sock_type_mask) == Constants::sock_stream ? Call::socket : Call::none;
        const Descriptor descriptor = kind(first);
        if (descriptor == Descriptor::tcp) return route.call;
        return route.call == Call::getsockopt && descriptor == Descriptor::udp ? route.call : Call::none;
    }
};
} // namespace ok::user::native
