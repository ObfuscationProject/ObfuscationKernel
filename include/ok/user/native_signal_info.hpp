#pragma once

#include "ok/sched/pending_signals.hpp"
#include "ok/user/native_signal_mask.hpp"

namespace ok::user::native
{

// Linux 128-byte siginfo wire record. Admission and user copy remain the
// caller's transactions; encoding is static, target-endian, and pointer-free.
template <abi::linux_user::Profile Profile>
struct SignalInfoCodec
{
    using Abi = abi::linux_user::Codec<Profile>;
    using Mask = SignalMaskCodec<Profile>;
    static constexpr usize bytes = 128;
    static constexpr u32 maximum_signal = Mask::mips ? 128 : 64;
    static constexpr usize code_offset = Mask::mips ? 4 : 8;
    static constexpr usize pid_offset = Abi::profile.word_bytes == 8 ? 16 : 12;
    static constexpr usize uid_offset = pid_offset + 4, status_offset = pid_offset + 8;
    static constexpr usize utime_offset = Abi::profile.word_bytes == 8 ? 32 : 24;
    static constexpr usize stime_offset = utime_offset + Abi::profile.word_bytes;

    template <typename Info>
    [[nodiscard]] static constexpr bool valid(const Info &info)
    {
        const sched::PendingSignalInfo value{static_cast<u32>(info.signal), info.code,
                                            info.sender_pid, info.sender_uid, info.child_status};
        return value.valid(maximum_signal);
    }
    // Preserve existing frame-specific SI_KERNEL admission. Synchronous wait
    // accepts every valid zero-payload SI_KERNEL record, not just FPE.
    template <bool AllowKernelFpe = false, typename Info>
    [[nodiscard]] static constexpr bool frame_valid(const Info &info)
    {
        return valid(info) && (info.code != 128 || (AllowKernelFpe && info.signal == 8));
    }
    template <typename Info>
    [[nodiscard]] static constexpr bool encode(const Info &info, std::array<std::byte, bytes> &out)
    {
        if (!valid(info)) return false;
        std::array<std::byte, bytes> next{};
        auto put = [&](usize at, u32 value) {
            for (usize i = 0; i < 4; ++i)
                next[at + i] = static_cast<std::byte>(value >> (8 * (Abi::profile.big_endian ? 3 - i : i)));
        };
        put(0, static_cast<u32>(info.signal));
        put(code_offset, static_cast<u32>(info.code));
        put(pid_offset, info.sender_pid);
        put(uid_offset, info.sender_uid);
        if (info.code == 1 || info.code == 2) put(status_offset, static_cast<u32>(info.child_status));
        // True user/system CPU accounting is not implemented by this private
        // prerequisite. Clock fields, unused union bytes and errno stay zero.
        out = next;
        return true;
    }
    template <typename Info, usize N>
    [[nodiscard]] static constexpr bool encode_into(const Info &info, std::array<std::byte, N> &out, usize offset)
    {
        if (offset > N || N - offset < bytes) return false;
        std::array<std::byte, bytes> next{};
        if (!encode(info, next)) return false;
        for (usize i = 0; i < bytes; ++i) out[offset + i] = next[i];
        return true;
    }
};

} // namespace ok::user::native
