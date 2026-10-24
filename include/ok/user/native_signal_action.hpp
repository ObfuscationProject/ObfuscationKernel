#pragma once

#include "ok/sched/signal_action.hpp"
#include "ok/user/native_signal_mask.hpp"
#include <concepts>

namespace ok::user::native
{

template <abi::linux_user::Profile Profile>
struct SignalActionCodec
{
    using Abi = abi::linux_user::Codec<Profile>;
    using Mask = SignalMaskCodec<Profile>;
    using Action = sched::SignalAction;
    static constexpr bool mips = Mask::mips;
    static constexpr usize word_bytes = Abi::profile.word_bytes;
    static constexpr bool big_endian = Abi::profile.big_endian;
    static constexpr bool has_restorer = Profile == abi::linux_user::Profile::i386 ||
        Profile == abi::linux_user::Profile::x86_64 || Profile == abi::linux_user::Profile::arm32 ||
        Profile == abi::linux_user::Profile::aarch64 || Profile == abi::linux_user::Profile::ppc;
    static constexpr usize absent_offset = ~usize{0};
    static constexpr usize handler_offset = mips ? word_bytes : 0;
    static constexpr usize flags_offset = mips ? 0 : word_bytes;
    static constexpr usize flags_bytes = mips ? 4 : word_bytes;
    static constexpr usize restorer_offset = has_restorer ? 2 * word_bytes : absent_offset;
    static constexpr usize mask_offset = (has_restorer ? 3 : 2) * word_bytes;
    static constexpr usize bytes = mask_offset + Mask::bytes;
    static constexpr u32 max_signal = mips ? 128 : 64;
    static constexpr u32 stop_signal = Mask::stop;
    static constexpr u32 child_signal = mips ? 18 : 17;
    static constexpr u64 no_child_stop = 1;
    static constexpr u64 no_child_wait = mips ? 0x10000 : 2;
    static constexpr u32 continue_signal = mips ? 25 : 18;
    static constexpr u32 window_signal = mips ? 20 : 28;
    static constexpr u32 urgent_signal = mips ? 21 : 23;
    // Linux UAPI_SA_FLAGS, not a delivery-support advertisement. ARM32's
    // SA_THIRTYTWO is UAPI; the same bit is internal SA_IA32_ABI on x86.
    static constexpr u64 uapi_flags = 0xd8000800ULL | (mips ? 0x00010009ULL : 0x00000007ULL) |
        (has_restorer ? 0x04000000ULL : 0) |
        (Profile == abi::linux_user::Profile::arm32 ? 0x02000000ULL : 0);
    static_assert((uapi_flags & 0x00000400ULL) == 0); // SA_UNSUPPORTED must be cleared.

    [[nodiscard]] static constexpr bool default_ignored(u32 signal)
    {
        return signal == child_signal || signal == continue_signal || signal == window_signal || signal == urgent_signal;
    }
    [[nodiscard]] static constexpr bool ignored(u32 signal, const Action &action)
    {
        return action.handler == Action::ignored_handler ||
            (action.handler == Action::default_handler && default_ignored(signal));
    }
    [[nodiscard]] static constexpr Action sanitize(Action action)
    {
        action.handler &= Abi::word_mask;
        action.flags &= uapi_flags;
        action.restorer = has_restorer ? action.restorer & Abi::word_mask : 0;
        action.mask = Mask::sanitize(action.mask);
        return action;
    }

    [[nodiscard]] static constexpr Action decode(const std::array<std::byte, bytes> &input)
    {
        Action action{};
        action.handler = read(input, handler_offset, word_bytes);
        action.flags = read(input, flags_offset, flags_bytes);
        if constexpr (has_restorer) action.restorer = read(input, restorer_offset, word_bytes);
        std::array<std::byte, Mask::bytes> mask{};
        for (usize i = 0; i < mask.size(); ++i) mask[i] = input[mask_offset + i];
        action.mask = Mask::decode(mask);
        return action;
    }
    [[nodiscard]] static constexpr std::array<std::byte, bytes> encode(const Action &action)
    {
        // Zero initialization also covers MIPS N64's flags-to-handler padding.
        std::array<std::byte, bytes> output{};
        write(output, handler_offset, word_bytes, action.handler);
        write(output, flags_offset, flags_bytes, action.flags);
        if constexpr (has_restorer) write(output, restorer_offset, word_bytes, action.restorer);
        const auto mask = Mask::encode(action.mask);
        for (usize i = 0; i < mask.size(); ++i) output[mask_offset + i] = mask[i];
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

// Internal action transaction. Native callers first admit the action through
// their architecture delivery policy and its implemented behavioral flags.
// Callers serialize table access. The statically bound discard callback cannot
// fail and must discard this signal from process and every thread's pending
// queues after commit, before copyout; it must not change the table itself.
// No default no-op callback is provided that could silently omit this effect.
template <abi::linux_user::Profile Profile, typename Read, typename Write, typename DiscardPending>
    requires requires(DiscardPending &discard, u32 signal) {
        { discard(signal) } noexcept -> std::same_as<void>;
    }
i64 update_signal_action(sched::SignalActionTable &table, i32 signal, u64 input, u64 output, u64 mask_size,
                         Read &&read, Write &&write, DiscardPending &&discard)
{
    using Codec = SignalActionCodec<Profile>;
    if (mask_size != Codec::Mask::bytes) return -22;
    sched::SignalAction requested{};
    if (input)
    {
        std::array<std::byte, Codec::bytes> bytes{};
        if (!read(input, std::span<std::byte>{bytes})) return -14;
        requested = Codec::decode(bytes);
    }
    if (signal < 1 || static_cast<u32>(signal) > Codec::max_signal ||
        (input && (signal == 9 || static_cast<u32>(signal) == Codec::stop_signal))) return -22;
    auto &current = *table.find(static_cast<u32>(signal));
    auto old = current;
    // Linux also strips non-UAPI flags from the old action returned to userland.
    old.flags &= Codec::uapi_flags;
    if (input)
    {
        current = Codec::sanitize(requested);
        if (Codec::ignored(static_cast<u32>(signal), current)) discard(static_cast<u32>(signal));
    }
    if (output)
    {
        const auto bytes = Codec::encode(old);
        if (!write(output, std::span<const std::byte>{bytes})) return -14;
    }
    return 0;
}

} // namespace ok::user::native
