#pragma once

#include "ok/abi/linux_user.hpp"

namespace ok::abi::linux_user
{

struct ConsoleIoctlRequest
{
    i32 fd{};
    ConsoleIoctlOperation operation{};
    u64 argument{};
};

// This codec only handles copied scalar words/bytes. The adapter owns descriptor
// authority, error ordering and checked user copies; no user pointer is followed.
template <Profile Target>
    requires SupportedProfile<Target>
struct ConsoleIoctlCodec final
{
    static constexpr usize pid_bytes = 4;

    [[nodiscard]] static constexpr std::optional<u32> number(ConsoleIoctlOperation operation)
    {
        const auto index = static_cast<usize>(operation);
        const auto &numbers = Codec<Target>::profile.console_ioctls;
        if (index >= numbers.size())
            return std::nullopt;
        return numbers[index];
    }

    [[nodiscard]] static constexpr std::optional<ConsoleIoctlOperation> operation(u64 command)
    {
        // Linux sys_ioctl takes an unsigned int request even on 64-bit ABIs.
        const u32 request = static_cast<u32>(command);
        const auto &numbers = Codec<Target>::profile.console_ioctls;
        for (usize index = 0; index < numbers.size(); ++index)
            if (numbers[index] == request)
                return static_cast<ConsoleIoctlOperation>(index);
        return std::nullopt;
    }

    [[nodiscard]] static constexpr i32 scalar_argument(u64 argument)
    {
        return std::bit_cast<i32>(static_cast<u32>(argument));
    }

    [[nodiscard]] static Result<ConsoleIoctlRequest> decode(std::span<const u64> arguments)
    {
        if (arguments.size() != 3)
            return Status::fault("Linux console ioctl argument frame is incomplete");
        const auto selected = operation(arguments[1]);
        if (!selected)
            return Status::not_terminal("Linux console ioctl request is outside the bounded profile");
        return ConsoleIoctlRequest{scalar_argument(arguments[0]), *selected,
                                   arguments[2] & Codec<Target>::word_mask};
    }

    [[nodiscard]] static Result<i32> decode_pid(std::span<const u8> bytes)
    {
        if (bytes.size() != pid_bytes)
            return Status::fault("Linux console ioctl PID copy must be exactly four bytes");
        u32 value{};
        for (usize index = 0; index < pid_bytes; ++index)
        {
            const usize offset = Codec<Target>::profile.big_endian ? pid_bytes - 1 - index : index;
            value |= static_cast<u32>(bytes[index]) << (offset * 8);
        }
        return std::bit_cast<i32>(value);
    }

    [[nodiscard]] static Status encode_pid(std::span<u8> bytes, i32 pid)
    {
        if (bytes.size() != pid_bytes)
            return Status::fault("Linux console ioctl PID copy must be exactly four bytes");
        const u32 value = std::bit_cast<u32>(pid);
        for (usize index = 0; index < pid_bytes; ++index)
        {
            const usize offset = Codec<Target>::profile.big_endian ? pid_bytes - 1 - index : index;
            bytes[index] = static_cast<u8>(value >> (offset * 8));
        }
        return Status::success();
    }
};

} // namespace ok::abi::linux_user
