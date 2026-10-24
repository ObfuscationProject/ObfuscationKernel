#pragma once

#include "ok/abi/linux_user.hpp"

namespace ok::user::native::baseline
{

template <abi::linux_user::Profile Selected>
concept KernelSignalTrampolineProfile =
    Selected == abi::linux_user::Profile::rv32 || Selected == abi::linux_user::Profile::rv64 ||
    Selected == abi::linux_user::Profile::mips || Selected == abi::linux_user::Profile::mips64 ||
    Selected == abi::linux_user::Profile::loongarch64;

enum class SignalTrampolineEntry : u8 { realtime, legacy };

// Bytes only: the caller supplies a reserved, kernel-owned RX user mapping and
// synchronizes instruction caches before publishing its entry addresses.
template <abi::linux_user::Profile Selected>
    requires KernelSignalTrampolineProfile<Selected>
struct SignalTrampoline
{
    using Codec = abi::linux_user::Codec<Selected>;
    using Operation = abi::linux_user::Operation;
    static constexpr bool mips = Selected == abi::linux_user::Profile::mips ||
                                 Selected == abi::linux_user::Profile::mips64;
    static constexpr bool riscv = Selected == abi::linux_user::Profile::rv32 ||
                                  Selected == abi::linux_user::Profile::rv64;
    static constexpr bool has_legacy = Selected == abi::linux_user::Profile::mips;
    static constexpr usize entry_bytes = 16, alignment = 4, realtime_offset = 0;
    static constexpr usize code_bytes = has_legacy ? 2 * entry_bytes : entry_bytes;
    static constexpr usize syscall_offset = 4, unexpected_return_offset = 8;
    static constexpr auto realtime_number = Codec::syscall_number(Operation::rt_sigreturn);
    static constexpr auto legacy_number = Codec::syscall_number(Operation::sigreturn);
    static_assert(realtime_number.has_value());
    static_assert(has_legacy == legacy_number.has_value());
    static_assert(*realtime_number <= (mips ? 32767U : 2047U));
    static_assert(!has_legacy || *legacy_number <= 32767U);
    static_assert(Codec::profile.big_endian == mips);
    static_assert(Codec::profile.number_register == (mips ? 2 : riscv ? 17 : 11));

    [[nodiscard]] static constexpr std::optional<usize> entry_offset(SignalTrampolineEntry entry)
    {
        if (entry == SignalTrampolineEntry::realtime) return realtime_offset;
        if (entry == SignalTrampolineEntry::legacy && has_legacy) return entry_bytes;
        return std::nullopt;
    }

    // Instructions have no branch delay slots, relocations or embedded PCs.
    // The last two instructions trap if a sigreturn syscall returns normally.
    [[nodiscard]] static constexpr std::array<std::byte, code_bytes> image()
    {
        std::array<std::byte, code_bytes> out{};
        const auto put = [&out](usize offset, u32 instruction) {
            for (usize i = 0; i < 4; ++i)
            {
                const usize shift = Codec::profile.big_endian ? 8 * (3 - i) : 8 * i;
                out[offset + i] = static_cast<std::byte>(instruction >> shift);
            }
        };
        const auto emit = [&put](usize offset, u32 number) {
            const u32 load = mips ? (0x24020000U | number) :
                             riscv ? ((number << 20) | (17U << 7) | 0x13U) :
                                     (0x02800000U | (number << 10) | 11U);
            const u32 syscall = mips ? 0x0000000cU : riscv ? 0x00000073U : 0x002b0000U;
            const u32 fault = mips ? 0x0000000dU : riscv ? 0x00100073U : 0x002a0000U;
            put(offset, load);
            put(offset + syscall_offset, syscall);
            put(offset + unexpected_return_offset, fault);
            put(offset + unexpected_return_offset + 4, fault);
        };
        emit(realtime_offset, *realtime_number);
        if constexpr (has_legacy) emit(entry_bytes, *legacy_number);
        return out;
    }

    // Numeric validation is not mapping, permission or lifetime validation.
    // Outputs remain unchanged on failure. No user address is chosen here.
    [[nodiscard]] static constexpr bool entry_address(u64 mapping_base, SignalTrampolineEntry entry,
                                                      u64 begin, u64 end, u64 &out)
    {
        const auto offset = entry_offset(entry);
        if (!offset || begin >= end || end > Codec::word_mask || mapping_base < begin ||
            mapping_base >= end || (mapping_base & (alignment - 1)) || code_bytes > end - mapping_base)
            return false;
        out = mapping_base + *offset;
        return true;
    }
};

} // namespace ok::user::native::baseline
