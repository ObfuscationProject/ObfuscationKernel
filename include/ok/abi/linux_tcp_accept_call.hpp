#pragma once
#include <ok/abi/linux_tcp_data.hpp>

namespace ok::abi::linux_user
{
struct TcpCallRoute
{
    TcpCall operation{TcpCall::none};
    bool socketcall{};
    u64 array{};
    usize array_bytes{};
};

// Pure register/value helpers. These neither read userspace nor own a listener.
// The runtime separately captures the fd from one head copy, then copies tail.
template <Profile Selected> requires SupportedProfile<Selected>
class TcpAcceptCall final
{
    using Target = Codec<Selected>;
  public:
    [[nodiscard]] static TcpCallRoute route(const RegisterFile &saved)
    {
        const u64 number = saved.integer[Target::profile.number_register];
        if (number > 0xffffffffu) return {};
        const auto operation = tcp_call<Selected>(static_cast<u32>(number));
        if (operation != TcpCall::socketcall) return {operation, false, 0, 0};
        const u64 selector = saved.integer[Target::profile.argument_registers[0]];
        const u64 array = saved.integer[Target::profile.argument_registers[1]];
        if (selector > Target::word_mask || array > Target::word_mask) return {};
        if (selector == 5) return {TcpCall::accept, true, array, 12};
        if (selector == 18) return {TcpCall::accept4, true, array, 16};
        return {};
    }
    [[nodiscard]] static Status interval(const TcpCallRoute &route)
    {
        if (!socket_route(route)) return Status::invalid_argument("TCP accept route is not a selected legacy subcall");
        return !route.array || route.array > Target::word_mask - (route.array_bytes - 1)
            ? Status::fault("TCP accept socketcall array interval wraps or is null") : Status{};
    }
    [[nodiscard]] static Result<i32> head(const TcpCallRoute &route, std::span<const std::byte> copied)
    {
        if (auto status = interval(route); !status) return status;
        if (copied.size() != 4) return Status::invalid_argument("TCP socketcall fd head requires exactly one word");
        return static_cast<i32>(word(copied));
    }
    [[nodiscard]] static Result<std::array<u64, 6>> tail(const TcpCallRoute &route, i32 fd,
        std::span<const std::byte> copied)
    {
        if (auto status = interval(route); !status) return status;
        if (copied.size() != route.array_bytes - 4) return Status::invalid_argument("TCP socketcall tail has another signature");
        std::array<u64, 6> arguments{static_cast<u32>(fd), 0, 0, 0, 0, 0};
        for (usize index = 0; index < copied.size() / 4; ++index)
            arguments[index + 1] = word(copied.subspan(index * 4, 4));
        return arguments;
    }
  private:
    static bool socket_route(const TcpCallRoute &route)
    {
        return tcp_socketcall_numbers[static_cast<usize>(Selected)] != unavailable_number &&
            Target::profile.word_bytes == 4 && route.socketcall &&
            ((route.operation == TcpCall::accept && route.array_bytes == 12) ||
             (route.operation == TcpCall::accept4 && route.array_bytes == 16));
    }
    static u32 word(std::span<const std::byte> bytes)
    {
        u32 value{};
        for (usize index = 0; index < 4; ++index)
        {
            const usize shift = Target::profile.big_endian ? (3 - index) * 8 : index * 8;
            value |= static_cast<u32>(std::to_integer<u8>(bytes[index])) << shift;
        }
        return value;
    }
};
}
