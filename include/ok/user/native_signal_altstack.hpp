#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/sched/signal_altstack.hpp"

namespace ok::user::native
{

template <abi::linux_user::Profile Profile>
struct SignalAltstackCodec
{
    using Abi = abi::linux_user::Codec<Profile>;
    using Stack = sched::SignalAltstack;
    static constexpr bool mips = Profile == abi::linux_user::Profile::mips ||
        Profile == abi::linux_user::Profile::mips64;
    static constexpr usize word_bytes = Abi::profile.word_bytes;
    static constexpr bool big_endian = Abi::profile.big_endian;
    static constexpr usize bytes = 3 * word_bytes;
    static constexpr usize base_offset = 0;
    static constexpr usize flags_offset = mips ? 2 * word_bytes : word_bytes;
    static constexpr usize size_offset = mips ? word_bytes : 2 * word_bytes;
    static constexpr u64 minimum = Profile == abi::linux_user::Profile::aarch64 ? 5120 :
        Profile == abi::linux_user::Profile::loongarch64 ? 4096 : 2048;

    [[nodiscard]] static constexpr Stack decode(const std::array<std::byte, bytes> &input)
    {
        return {read(input, base_offset, word_bytes), read(input, size_offset, word_bytes),
                static_cast<u32>(read(input, flags_offset, 4))};
    }
    [[nodiscard]] static constexpr std::array<std::byte, bytes> encode(const Stack &state)
    {
        // Query padding must be zero; 64-bit MIPS padding follows flags, while
        // other 64-bit profiles pad between flags and size.
        std::array<std::byte, bytes> output{};
        write(output, base_offset, word_bytes, state.base);
        write(output, size_offset, word_bytes, state.size);
        write(output, flags_offset, 4, state.flags);
        return output;
    }

  private:
    static constexpr u64 read(const std::array<std::byte, bytes> &input, usize at, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i)
            value |= static_cast<u64>(input[at + i]) << (8 * (big_endian ? count - 1 - i : i));
        return value;
    }
    static constexpr void write(std::array<std::byte, bytes> &output, usize at, usize count, u64 value)
    {
        for (usize i = 0; i < count; ++i)
            output[at + i] = static_cast<std::byte>(value >> (8 * (big_endian ? count - 1 - i : i)));
    }
};

// The caller supplies normalized ABI addresses/SP, serializes thread state,
// and implements user copies that cannot mutate this state or its size policy.
// Only the stack_t record is copied; ss_sp is deliberately not dereferenced or
// eagerly checked for mapping, alignment, canonicality or overflow here.
template <abi::linux_user::Profile Profile, typename Read, typename Write, typename SizeValid>
i64 update_signal_altstack(sched::SignalAltstack &state, u64 input, u64 output, u64 sp,
                          Read &&read, Write &&write, SizeValid &&valid)
{
    using Codec = SignalAltstackCodec<Profile>;
    sched::SignalAltstack requested{};
    if (input)
    {
        std::array<std::byte, Codec::bytes> bytes{};
        if (!read(input, std::span<std::byte>{bytes})) return -14;
        requested = Codec::decode(bytes);
    }
    const auto old = state.query(sp);
    if (input)
    {
        const auto error = state.apply(requested, sp, Codec::minimum, valid);
        if (error) return error;
    }
    if (output)
    {
        const auto bytes = Codec::encode(old);
        if (!write(output, std::span<const std::byte>{bytes})) return -14;
    }
    return 0;
}

// Apply stack bytes already staged by a profile-specific signal-frame decoder.
// Linux restore_altstack suppresses EPERM/EINVAL/ENOMEM and preserves old state
// in those cases. Do not reject an otherwise valid frame for semantic errors.
template <abi::linux_user::Profile Profile, typename SizeValid>
void apply_restored_altstack(sched::SignalAltstack &state, const sched::SignalAltstack &staged,
                            u64 restored_sp, SizeValid &&valid) noexcept
{
    (void)state.apply(staged, restored_sp, SignalAltstackCodec<Profile>::minimum, valid);
}

// Standalone copy/restore operation. Unlike the syscall's null query pointer,
// address zero still attempts a copy and must report EFAULT on copy failure.
// Runtime sigreturn should stage this copy with all register/FP/mask input and
// only then call apply_restored_altstack as part of its successful commit.
template <abi::linux_user::Profile Profile, typename Read, typename SizeValid>
i64 restore_signal_altstack(sched::SignalAltstack &state, u64 address, u64 restored_sp,
                           Read &&read, SizeValid &&valid)
{
    using Codec = SignalAltstackCodec<Profile>;
    std::array<std::byte, Codec::bytes> bytes{};
    if (!read(address, std::span<std::byte>{bytes})) return -14;
    apply_restored_altstack<Profile>(state, Codec::decode(bytes), restored_sp, valid);
    return 0;
}

} // namespace ok::user::native
