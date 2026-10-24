#pragma once

#include "ok/abi/linux_udp_address.hpp"
#include "ok/abi/linux_udp_data.hpp"
#include "ok/abi/linux_udp_name_data.hpp"
#include "ok/abi/linux_udp_connected_data.hpp"

namespace ok::abi::linux_user
{

enum class UdpCall : u8 { socket, bind, sendto, recvfrom, getsockname, connect, getpeername };

// Target values only. A request carries no user-memory or endpoint authority.
struct UdpRequest
{
    UdpCall call{UdpCall::socket};
    i32 fd{}, domain{}, type{}, protocol{};
    u64 payload{}, count{};
    u32 flags{};
    u64 address{};
    i32 address_length{};
    u64 source_length{}; // In/out socklen pointer for receive or name queries.
    [[nodiscard]] bool operator==(const UdpRequest &) const = default;
};

template <typename Copy>
concept UdpArgumentCopy = requires(Copy &copy, u64 address, std::span<u8> bytes)
{
    { copy(address, bytes) } -> std::same_as<Status>;
};

template <Profile Selected>
    requires SupportedProfile<Selected>
class UdpConstants final
{
  public:
    static constexpr const UdpProfileData &data = udp_profiles[static_cast<usize>(Selected)];
    static constexpr u32 af_unspec = udp_af_unspec;
    static constexpr u32 af_inet = udp_af_inet;
    static constexpr u32 protocol_udp = udp_protocol;
    static constexpr u32 sock_stream = data.socket[0];
    static constexpr u32 sock_dgram = data.socket[1];
    static constexpr u32 sock_nonblock = data.socket[2];
    static constexpr u32 sock_cloexec = data.socket[3];
    static constexpr u32 sock_type_mask = data.socket[4];
    static constexpr usize sockaddr_input_limit = udp_sockaddr_input_limit;

    [[nodiscard]] static constexpr std::optional<u32> syscall_number(UdpCall call)
    {
        if (call == UdpCall::getsockname)
            return udp_getsockname_numbers[static_cast<usize>(Selected)];
        if (call == UdpCall::connect)
            return udp_connect_numbers[static_cast<usize>(Selected)];
        if (call == UdpCall::getpeername)
            return udp_getpeername_numbers[static_cast<usize>(Selected)];
        const usize index = static_cast<usize>(call);
        if (index >= 4 || data.numbers[index] == unavailable_number) return std::nullopt;
        return data.numbers[index];
    }
    [[nodiscard]] static constexpr std::optional<u32> socketcall_number()
    {
        return data.numbers[4] == unavailable_number ? std::nullopt : std::optional<u32>{data.numbers[4]};
    }
    [[nodiscard]] static constexpr std::optional<u32> message(UdpMessage flag)
    {
        const usize index = static_cast<usize>(flag);
        return index < data.messages.size() ? std::optional<u32>{data.messages[index]} : std::nullopt;
    }
    [[nodiscard]] static constexpr std::optional<u32> errno_number(UdpErrno error)
    {
        const usize index = static_cast<usize>(error);
        return index < data.errors.size() ? std::optional<u32>{data.errors[index]} : std::nullopt;
    }
    [[nodiscard]] static constexpr u32 not_connected_errno_number()
    {
        return udp_enotconn_numbers[static_cast<usize>(Selected)];
    }
    [[nodiscard]] static constexpr u32 send_flags_mask()
    {
        return *message(UdpMessage::dontwait) | *message(UdpMessage::nosignal);
    }
    [[nodiscard]] static constexpr u32 receive_flags_mask()
    {
        return *message(UdpMessage::dontwait) | *message(UdpMessage::peek) | *message(UdpMessage::trunc);
    }
    // Decoder failures only; operation/usercopy status translation is separate.
    [[nodiscard]] static constexpr std::optional<u32> decode_error_number(StatusCode code)
    {
        switch (code)
        {
        case StatusCode::invalid_argument: return errno_number(UdpErrno::e_inval);
        case StatusCode::fault: return errno_number(UdpErrno::e_fault);
        case StatusCode::overflow: return errno_number(UdpErrno::e_overflow);
        case StatusCode::unsupported: return errno_number(UdpErrno::e_opnotsupp);
        case StatusCode::not_found: return errno_number(UdpErrno::e_nosys);
        default: return std::nullopt;
        }
    }
};

template <Profile Selected>
    requires SupportedProfile<Selected>
class UdpCallDecoder final
{
    using Constants = UdpConstants<Selected>;
    using Target = Codec<Selected>;

  public:
    [[nodiscard]] static Result<UdpRequest> decode_direct(const Call &raw)
    {
        for (usize index = 0; index < 7; ++index)
        {
            const auto call = static_cast<UdpCall>(index);
            if (Constants::syscall_number(call) == raw.number) return normalize(call, raw);
        }
        return Status::not_found("Linux UDP direct syscall number is outside this decoder");
    }

    [[nodiscard]] static Result<UdpRequest> decode_direct(const RegisterFile &frame,
                                                         std::span<const u8> copied_stack = {})
    {
        const u64 number = frame.integer[Target::profile.number_register];
        if (number > 0xffffffffU) return Status::overflow("Linux UDP syscall snapshot exceeds 32 bits");
        for (usize index = 0; index < 7; ++index)
        {
            const auto call = static_cast<UdpCall>(index);
            if (Constants::syscall_number(call) != number) continue;
            const auto raw = Target::decode_call(frame, arguments(call), copied_stack);
            return raw ? normalize(call, raw.value()) : Result<UdpRequest>{raw.status()};
        }
        return Status::not_found("Linux UDP direct syscall number is outside this decoder");
    }

    // Copy owns access permission; this decoder never dereferences an address.
    // The callable must not retain the destination span after returning.
    template <UdpArgumentCopy Copy>
    [[nodiscard]] static Result<UdpRequest> decode_socketcall(const Call &raw, Copy &copy)
    {
        const auto number = Constants::socketcall_number();
        if (!number || raw.number != *number)
            return Status::not_found("Linux UDP socketcall is absent or has another number");
        if (auto status = validate_words(raw, 2); !status) return status;
        const i32 selector = signed32(raw.arguments[0]);
        if (selector < 1 || selector > 20)
            return Status::invalid_argument("Linux socketcall selector is out of range");
        std::optional<UdpCall> selected{};
        if (static_cast<u32>(selector) == udp_getsockname_subcall)
            selected = UdpCall::getsockname;
        if (static_cast<u32>(selector) == udp_connect_subcall)
            selected = UdpCall::connect;
        if (static_cast<u32>(selector) == udp_getpeername_subcall)
            selected = UdpCall::getpeername;
        for (usize index = 0; index < udp_subcalls.size(); ++index)
            if (udp_subcalls[index] == static_cast<u32>(selector)) selected = static_cast<UdpCall>(index);
        if (!selected) return Status::unsupported("Linux socketcall operation is outside the UDP slice");
        const UdpCall call = *selected;
        const usize words = arguments(call);
        const usize bytes = words * Target::profile.word_bytes;
        const u64 address = raw.arguments[1];
        if (address > Target::word_mask - (bytes - 1))
            return Status::fault("Linux socketcall argument interval wraps the target word");
        std::array<u8, 6 * 8> copied{};
        if (auto status = copy(address, std::span<u8>{copied}.first(bytes)); !status) return status;
        Call decoded{*Constants::syscall_number(call), {}, words};
        for (usize index = 0; index < words; ++index)
            decoded.arguments[index] = read_word(std::span<const u8>{copied}.subspan(
                index * Target::profile.word_bytes, Target::profile.word_bytes));
        return normalize(call, decoded);
    }

    template <UdpArgumentCopy Copy>
    [[nodiscard]] static Result<UdpRequest> decode_socketcall(const RegisterFile &frame, Copy &copy)
    {
        const auto number = Constants::socketcall_number();
        if (!number || frame.integer[Target::profile.number_register] != *number)
            return Status::not_found("Linux UDP socketcall is absent or has another number");
        const auto raw = Target::decode_call(frame, 2);
        return raw ? decode_socketcall(raw.value(), copy) : Result<UdpRequest>{raw.status()};
    }

  private:
    [[nodiscard]] static constexpr usize arguments(UdpCall call)
    {
        return call == UdpCall::sendto || call == UdpCall::recvfrom ? 6 : 3;
    }
    [[nodiscard]] static Status validate_words(const Call &raw, usize required)
    {
        if (raw.argument_count > raw.arguments.size() || raw.argument_count < required)
            return Status::invalid_argument("Linux UDP argument snapshot is incomplete or malformed");
        for (usize index = 0; index < required; ++index)
            if (raw.arguments[index] > Target::word_mask)
                return Status::overflow("Linux UDP argument exceeds the target word snapshot");
        return Status::success();
    }
    [[nodiscard]] static constexpr i32 signed32(u64 value)
    {
        return std::bit_cast<i32>(static_cast<u32>(value));
    }
    [[nodiscard]] static u64 read_word(std::span<const u8> bytes)
    {
        u64 value{};
        for (usize index = 0; index < bytes.size(); ++index)
        {
            const usize shift = Target::profile.big_endian ? (bytes.size() - index - 1) * 8 : index * 8;
            value |= u64{bytes[index]} << shift;
        }
        return value;
    }
    [[nodiscard]] static Result<UdpRequest> normalize(UdpCall call, const Call &raw)
    {
        if (auto status = validate_words(raw, arguments(call)); !status) return status;
        const auto &a = raw.arguments;
        UdpRequest result{.call = call};
        switch (call)
        {
        case UdpCall::socket:
            result.domain = signed32(a[0]); result.type = signed32(a[1]); result.protocol = signed32(a[2]);
            break;
        case UdpCall::bind:
        case UdpCall::connect:
            result.fd = signed32(a[0]); result.address = a[1]; result.address_length = signed32(a[2]);
            break;
        case UdpCall::getsockname:
        case UdpCall::getpeername:
            result.fd = signed32(a[0]); result.address = a[1]; result.source_length = a[2];
            break;
        case UdpCall::sendto:
        case UdpCall::recvfrom:
            result.fd = signed32(a[0]); result.payload = a[1]; result.count = a[2];
            result.flags = static_cast<u32>(a[3]); result.address = a[4];
            if (call == UdpCall::sendto) result.address_length = signed32(a[5]);
            else result.source_length = a[5];
            break;
        }
        return result;
    }
};

template <Profile Selected>
    requires SupportedProfile<Selected>
class UdpWire final
{
  public:
    [[nodiscard]] static Result<u16> decode_family(std::span<const u8> copied)
    {
        if (copied.size() < 2 || copied.size() > UdpConstants<Selected>::sockaddr_input_limit)
            return Status::invalid_argument("Linux UDP copied family length is outside bounds");
        return Codec<Selected>::profile.big_endian
            ? static_cast<u16>((u16{copied[0]} << 8) | copied[1])
            : static_cast<u16>((u16{copied[1]} << 8) | copied[0]);
    }
    [[nodiscard]] static Result<Ipv4SocketAddress> decode_address(std::span<const u8> copied)
    {
        if (copied.size() < Ipv4SocketAddressCodec<Selected>::record_size ||
            copied.size() > UdpConstants<Selected>::sockaddr_input_limit)
            return Status::invalid_argument("Linux UDP copied address length is outside IPv4 bounds");
        return Ipv4SocketAddressCodec<Selected>::decode(copied.first(16));
    }
    [[nodiscard]] static Status encode_address(std::span<u8> bytes, Ipv4SocketAddress value)
    {
        return Ipv4SocketAddressCodec<Selected>::encode(bytes, value);
    }
    // Negative capacities are preserved; their operation-specific timing is later.
    [[nodiscard]] static Result<i32> decode_socklen(std::span<const u8> bytes)
    {
        if (bytes.size() != 4) return Status::invalid_argument("Linux socklen copied record must be four bytes");
        u32 value{};
        for (usize index = 0; index < 4; ++index)
            value |= u32{bytes[index]} << (Codec<Selected>::profile.big_endian ? (3 - index) * 8 : index * 8);
        return std::bit_cast<i32>(value);
    }
    [[nodiscard]] static Status encode_socklen(std::span<u8> bytes, u32 actual)
    {
        if (bytes.size() != 4) return Status::invalid_argument("Linux socklen copied record must be four bytes");
        if (actual > 0x7fffffffU) return Status::overflow("Linux socklen actual size exceeds signed32");
        for (usize index = 0; index < 4; ++index)
            bytes[index] = static_cast<u8>(actual >> (Codec<Selected>::profile.big_endian ? (3 - index) * 8 : index * 8));
        return Status::success();
    }
};

} // namespace ok::abi::linux_user
