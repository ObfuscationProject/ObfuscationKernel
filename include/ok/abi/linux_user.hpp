#pragma once

#include "ok/abi/linux_profile_data.hpp"

#include <bit>
#include <span>

namespace ok::abi::linux_user
{

// This snapshot is independent of all native trap structures. x86 uses DWARF
// integer register indices; other profiles use ISA integer register numbers.
struct RegisterFile
{
    std::array<u64, 32> integer{};
    u32 condition{0}; // PowerPC CR, including CR0.SO at bit 28.
};

struct Call
{
    u32 number{0};
    std::array<u64, 6> arguments{};
    usize argument_count{0};
};

struct Return
{
    u64 value{0};
    u32 error{0}; // Positive architecture errno; zero means success.
};

struct Timespec
{
    i64 seconds{0};
    i64 nanoseconds{0};
};

enum class TimeFormat : u8
{
    legacy_timespec, // Two target longs, not musl's public time64 struct.
    kernel_timespec, // Linux __kernel_timespec: two 64-bit fields; 32-bit input ignores high tv_nsec padding.
};

template <Profile Selected>
concept SupportedProfile = static_cast<usize>(Selected) < static_cast<usize>(Profile::count);

template <Profile Selected>
    requires SupportedProfile<Selected>
class Codec final
{
  public:
    static constexpr const ProfileData &profile = profiles[static_cast<usize>(Selected)];
    static constexpr u64 word_mask = profile.word_bytes == 4 ? u64{0xffffffffU} : ~u64{0};
    static constexpr u32 ppc_error_bit = 0x10000000U;

    [[nodiscard]] static constexpr std::optional<u32> syscall_number(Operation operation)
    {
        const auto index = static_cast<usize>(operation);
        if (index >= profile.numbers.size() || profile.numbers[index] == unavailable_number)
            return std::nullopt;
        return profile.numbers[index];
    }

    [[nodiscard]] static constexpr std::optional<u32> errno_number(Error error)
    {
        const auto index = static_cast<usize>(error);
        if (index >= profile.errors.size())
            return std::nullopt;
        return profile.errors[index];
    }

    [[nodiscard]] static constexpr usize timespec_size(TimeFormat format)
    {
        return format == TimeFormat::kernel_timespec ? 16 : 2 * profile.word_bytes;
    }

    [[nodiscard]] static Result<Timespec> decode_timespec(std::span<const u8> bytes, TimeFormat format)
    {
        if (!valid_format(format) || bytes.size() != timespec_size(format))
            return Status::invalid_argument("Linux timespec size or format");
        const usize width = bytes.size() / 2;
        // Linux get_timespec64 drops the high tv_nsec padding on a 32-bit
        // syscall ABI. Decode integer significance, independent of byte order.
        const usize nsec_width = profile.word_bytes == 4 ? 4 : width;
        const Timespec result{signed_value(read_word(bytes.first(width)), width),
                              signed_value(read_word(bytes.subspan(width)), nsec_width)};
        if (!valid_nanoseconds(result.nanoseconds))
            return Status::invalid_argument("Linux timespec nanoseconds");
        return result;
    }

    [[nodiscard]] static Status encode_timespec(std::span<u8> bytes, TimeFormat format, Timespec value)
    {
        if (!valid_format(format) || bytes.size() != timespec_size(format) || !valid_nanoseconds(value.nanoseconds))
            return Status::invalid_argument("Linux timespec size, format or nanoseconds");
        const usize width = bytes.size() / 2;
        if (width == 4 && (value.seconds < -2147483648LL || value.seconds > 2147483647LL))
            return Status::overflow("Linux legacy time32 seconds");
        // Validate first: failed conversion must not partially publish a user record.
        write_word(bytes.first(width), static_cast<u64>(value.seconds));
        write_word(bytes.subspan(width), static_cast<u64>(value.nanoseconds));
        return Status::success();
    }

    // The caller must copy stack bytes under its user-access/lifetime contract.
    // This function never dereferences a user address and never loads a syscall.
    [[nodiscard]] static Result<Call> decode_call(const RegisterFile &frame, usize count,
                                                std::span<const u8> stack_from_sp = {})
    {
        if (count > 6)
            return Status::unsupported("Linux codec supports at most six raw arguments");
        const u64 number = frame.integer[profile.number_register];
        if (number > 0xffffffffU)
            return Status::overflow("noncanonical Linux syscall number snapshot");
        Call call{static_cast<u32>(number), {}, count};
        for (usize index = 0; index < count; ++index)
        {
            const i8 register_index = profile.argument_registers[index];
            u64 value = 0;
            if (register_index >= 0)
            {
                value = frame.integer[static_cast<usize>(register_index)];
                if (value > word_mask)
                    return Status::overflow("noncanonical Linux register word snapshot");
            }
            else
            {
                // Linux MIPS O32 reserves the first four argument home slots.
                const usize offset = index * 4;
                if (stack_from_sp.size() < offset + 4)
                    return Status::fault("copied Linux MIPS O32 stack arguments are incomplete");
                value = read_word(stack_from_sp.subspan(offset, 4));
            }
            call.arguments[index] = value;
        }
        return call;
    }

    // The signature-specific caller selects the first slot. This does not infer
    // padding or reordered argument signatures from a syscall number.
    [[nodiscard]] static Result<u64> decode_argument64(const Call &call, usize first)
    {
        if (call.argument_count > call.arguments.size() || first >= call.argument_count)
            return Status::invalid_argument("Linux argument index");
        if constexpr (profile.word_bytes == 8)
            return call.arguments[first];
        if (profile.aligned_64bit_pairs && (first & 1U) != 0)
            return Status::invalid_argument("Linux 64-bit argument requires an even slot");
        if (first + 1 >= call.argument_count)
            return Status::invalid_argument("Linux 64-bit argument pair is incomplete");
        const u64 one = call.arguments[first];
        const u64 two = call.arguments[first + 1];
        if (one > word_mask || two > word_mask)
            return Status::overflow("Linux argument pair exceeds target word size");
        return profile.big_endian ? (one << 32) | two : (two << 32) | one;
    }

    [[nodiscard]] static Result<Return> decode_return(const RegisterFile &frame)
    {
        const u64 value = frame.integer[profile.return_register];
        if (value > word_mask)
            return Status::overflow("noncanonical Linux return word snapshot");
        if constexpr (profile.error_convention == ErrorConvention::negative)
        {
            if (value >= word_mask - 4094)
                return Return{0, static_cast<u32>(word_mask - value + 1)};
            return Return{value, 0};
        }
        bool error = false;
        if constexpr (profile.error_convention == ErrorConvention::mips_a3)
        {
            if (frame.integer[7] > 1)
                return Status::invalid_argument("noncanonical Linux MIPS a3 return flag");
            error = frame.integer[7] != 0;
        }
        else
            error = (frame.condition & ppc_error_bit) != 0;
        if (!error)
            return Return{value, 0};
        if (value == 0 || value > 4095)
            return Status::invalid_argument("Linux flagged return has invalid errno");
        return Return{0, static_cast<u32>(value)};
    }

    [[nodiscard]] static Status encode_return(RegisterFile &frame, Return result)
    {
        if (result.error > 4095 || (result.error != 0 && result.value != 0))
            return Status::invalid_argument("Linux return must be success or one positive errno");
        if (result.value > word_mask)
            return Status::overflow("Linux result exceeds target word size");
        if constexpr (profile.error_convention == ErrorConvention::negative)
        {
            if (result.error == 0 && result.value >= word_mask - 4094)
                return Status::invalid_argument("successful Linux value collides with reserved errno range");
            frame.integer[profile.return_register] = result.error != 0 ? word_mask - result.error + 1 : result.value;
        }
        else
        {
            frame.integer[profile.return_register] = result.error != 0 ? result.error : result.value;
            if constexpr (profile.error_convention == ErrorConvention::mips_a3)
                frame.integer[7] = result.error != 0 ? 1 : 0;
            else
                frame.condition = (frame.condition & ~ppc_error_bit) | (result.error != 0 ? ppc_error_bit : 0U);
        }
        return Status::success();
    }

  private:
    [[nodiscard]] static constexpr bool valid_format(TimeFormat format)
    {
        return format == TimeFormat::legacy_timespec || format == TimeFormat::kernel_timespec;
    }

    [[nodiscard]] static constexpr bool valid_nanoseconds(i64 value)
    {
        return value >= 0 && value < 1000000000;
    }

    [[nodiscard]] static constexpr u64 read_word(std::span<const u8> bytes)
    {
        u64 result = 0;
        for (usize index = 0; index < bytes.size(); ++index)
        {
            const usize shift = profile.big_endian ? (bytes.size() - index - 1) * 8 : index * 8;
            result |= static_cast<u64>(bytes[index]) << shift;
        }
        return result;
    }

    static constexpr void write_word(std::span<u8> bytes, u64 value)
    {
        for (usize index = 0; index < bytes.size(); ++index)
        {
            const usize shift = profile.big_endian ? (bytes.size() - index - 1) * 8 : index * 8;
            bytes[index] = static_cast<u8>(value >> shift);
        }
    }

    [[nodiscard]] static constexpr i64 signed_value(u64 value, usize width)
    {
        return width == 4 ? static_cast<i64>(std::bit_cast<i32>(static_cast<u32>(value))) : std::bit_cast<i64>(value);
    }
};

} // namespace ok::abi::linux_user
