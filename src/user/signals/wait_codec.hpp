#pragma once

#include "ok/sched/pending_signals.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"

namespace ok::user::native
{
enum class SignalWaitKind : u8 { none, suspend, timed };

template <abi::linux_user::Profile Profile>
struct SignalWaitCodec
{
    using Abi = abi::linux_user::Codec<Profile>;
    using Mask = SignalMaskCodec<Profile>;
    using Operation = abi::linux_user::Operation;
    using TimeFormat = abi::linux_user::TimeFormat;
    struct Request
    {
        SignalWaitKind kind{};
        sched::SignalMask wanted{};
        u64 info{};
        bool has_timeout{};
        abi::linux_user::Timespec timeout{};
    };

    [[nodiscard]] static constexpr bool is(u32 number, Operation operation)
    {
        const auto expected = Abi::syscall_number(operation);
        return expected && number == *expected;
    }
    [[nodiscard]] static constexpr SignalWaitKind kind(u32 number)
    {
        if (is(number, Operation::rt_sigsuspend)) return SignalWaitKind::suspend;
        if (is(number, Operation::rt_sigtimedwait) || is(number, Operation::rt_sigtimedwait_time64))
            return SignalWaitKind::timed;
        return SignalWaitKind::none;
    }
    [[nodiscard]] static constexpr TimeFormat format(u32 number)
    {
        return is(number, Operation::rt_sigtimedwait_time64) || Abi::profile.word_bytes == 8
            ? TimeFormat::kernel_timespec : TimeFormat::legacy_timespec;
    }

    // A failed input transaction leaves the prior output and all pending state
    // untouched. The caller supplies a canonical architecture-decoded call.
    template <typename Read>
    [[nodiscard]] static i64 decode(const abi::linux_user::Call &call, Read &&read, Request &out)
    {
        const auto operation = kind(call.number);
        if (operation == SignalWaitKind::none) return -95;
        const usize arguments = operation == SignalWaitKind::suspend ? 2 : 4;
        if (call.argument_count < arguments || call.argument_count > call.arguments.size()) return -22;
        if (call.arguments[arguments - 1] != Mask::bytes) return -22;
        std::array<std::byte, Mask::bytes> bytes{};
        if (!read(call.arguments[0], std::span<std::byte>{bytes})) return -14;
        Request next{};
        next.kind = operation;
        next.wanted = Mask::sanitize(Mask::decode(bytes));
        if (operation == SignalWaitKind::timed)
        {
            next.info = call.arguments[1];
            next.has_timeout = call.arguments[2] != 0;
            if (next.has_timeout)
            {
                std::array<std::byte, 16> input{};
                const auto count = Abi::timespec_size(format(call.number));
                if (!read(call.arguments[2], std::span<std::byte>{input}.first(count))) return -14;
                std::array<u8, 16> raw{};
                for (usize i = 0; i < count; ++i) raw[i] = std::to_integer<u8>(input[i]);
                const auto time = Abi::decode_timespec(std::span<const u8>{raw}.first(count), format(call.number));
                if (!time || time.value().seconds < 0) return -22;
                next.timeout = time.value();
            }
        }
        out = next;
        return 0;
    }

    [[nodiscard]] static constexpr sched::SignalMask selection_mask(sched::SignalMask wanted)
    {
        wanted = Mask::sanitize(wanted);
        for (auto &word : wanted.words) word = ~word;
        // Do not sanitize the complement: KILL/STOP must stay excluded.
        return wanted;
    }

    // Only current pending payload variants are encoded. Unused siginfo union
    // bytes are zero. The wait owner consumes before attempting user copyout.
    [[nodiscard]] static constexpr bool encode_info(sched::PendingSignalInfo info,
                                                    std::array<std::byte, 128> &out)
    {
        return SignalInfoCodec<Profile>::encode(info, out);
    }
};
} // namespace ok::user::native
