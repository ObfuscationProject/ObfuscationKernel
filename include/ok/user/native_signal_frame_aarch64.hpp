#pragma once

#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"
#include <concepts>

#if defined(__AARCH64EB__)
#error "The native AArch64 signal frame contract is little-endian only"
#endif

namespace ok::user::native
{

// Linux AArch64 little-endian RT frame for the current FPSIMD-only backend.
// This value codec does not implement delivery, usercopy or an rt_sigreturn.
struct Aarch64SignalFrame
{
    using Frame = arch::execution::UserFrame;
    using MaskCodec = SignalMaskCodec<abi::linux_user::Profile::aarch64>;
    using InfoCodec = SignalInfoCodec<abi::linux_user::Profile::aarch64>;
    static_assert(sizeof(uptr) == 8);
    static constexpr usize frame_bytes = 4688, record_bytes = 16, image_bytes = frame_bytes + record_bytes;
    static constexpr usize info_offset = 0, ucontext_offset = 128, stack_offset = 144;
    static constexpr usize mask_offset = 168, context_offset = 304, registers_offset = 312;
    static constexpr usize sp_offset = 560, pc_offset = 568, pstate_offset = 576;
    static constexpr usize reserved_offset = 592, reserved_bytes = 4096, fpsimd_bytes = 528;
    static constexpr u32 fpsimd_magic = 0x46508001, esr_magic = 0x45535201;
    static constexpr u64 nzcv_mask = 0xf0000000, user_ceiling = u64{1} << 48;

    struct Stack
    {
        u64 pointer{};
        u64 size{};
        u32 flags{2};
        constexpr bool operator==(const Stack &) const = default;
    };
    // Stack serialization is separate from future altstack ownership policy.
    // A replacement policy must validate/normalize without changing live state.
    struct DisabledStack
    {
        constexpr bool operator()(Stack supplied, Stack &normalized) const noexcept
        {
            if (supplied.flags != 2) return false;
            normalized = {};
            return true;
        }
    };
    struct Info { i32 signal{}; i32 code{}; u32 sender_pid{}; u32 sender_uid{}; i32 child_status{}; };
    static constexpr bool valid_info(Info info)
    {
        return InfoCodec::template frame_valid<>(info);
    }
    struct Placement
    {
        u64 address{};
        u64 record_address{};
        constexpr bool operator==(const Placement &) const = default;
    };
    struct Image { std::array<std::byte, image_bytes> bytes{}; };
    struct Restored { Frame frame{}; SignalMask mask{}; Stack stack{}; };

    static constexpr bool window(u64 begin, u64 end)
    {
        return begin < end && end <= user_ceiling;
    }
    static constexpr bool range(u64 address, u64 size, u64 begin, u64 end)
    {
        return window(begin, end) && address >= begin && address < end && size <= end - address;
    }
    static constexpr bool program_state(u64 pc, u64 pstate, u64 begin, u64 end)
    {
        // The backend currently retains NZCV only: no AArch32, privileged EL,
        // interrupt masking, single-step, BTI, MTE, PAN or other feature state.
        return range(pc, 4, begin, end) && !(pc & 3) && !(pstate & ~nzcv_mask);
    }
    static constexpr bool user_state(u64 pc, u64 sp, u64 pstate, u64 begin, u64 end)
    {
        return program_state(pc, pstate, begin, end) && sp > begin && sp <= end && !(sp & 15);
    }
    static constexpr bool place(u64 top, u64 begin, u64 end, Placement &out)
    {
        // AArch64 has no red zone. Linux reserves an AAPCS64 unwind record
        // above the fixed RT frame, with both addresses aligned to 16 bytes.
        if (!window(begin, end) || top <= begin || top > end || top - begin < record_bytes) return false;
        const u64 record = (top - record_bytes) & ~u64{15};
        if (record < begin || record - begin < frame_bytes) return false;
        const u64 address = record - frame_bytes;
        if (!range(address, image_bytes, begin, end)) return false;
        out = {address, record};
        return true;
    }

    template <usize N>
    static constexpr u64 get(const std::array<std::byte, N> &bytes, usize at, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i) value |= static_cast<u64>(bytes[at + i]) << (i * 8);
        return value;
    }
    template <usize N>
    static constexpr void put(std::array<std::byte, N> &bytes, usize at, u64 value, usize count)
    {
        for (usize i = 0; i < count; ++i) bytes[at + i] = static_cast<std::byte>(value >> (i * 8));
    }

    template <typename StackPolicy = DisabledStack>
        requires requires(const StackPolicy &policy, Stack stack, Stack &normalized) {
            { policy(stack, normalized) } noexcept -> std::same_as<bool>;
        }
    static bool encode(const Frame &frame, SignalMask mask, Stack stack, Info info,
                       u64 begin, u64 end, Image &out, const StackPolicy &policy = {})
    {
        Stack normalized;
        // Captured SP is saved verbatim so a handler on a valid alternate stack
        // can repair it. Handler placement and restored SP are checked separately.
        if (!program_state(frame.pc, frame.flags, begin, end) ||
            !valid_info(info) ||
            !policy(stack, normalized)) return false;
        Image image;
        if (!InfoCodec::encode_into(info, image.bytes, info_offset)) return false;
        put(image.bytes, stack_offset, normalized.pointer, 8);
        put(image.bytes, stack_offset + 8, normalized.flags, 4);
        put(image.bytes, stack_offset + 16, normalized.size, 8);
        mask = MaskCodec::sanitize(mask);
        put(image.bytes, mask_offset, mask.words[0], 8);
        for (usize i = 0; i < 31; ++i) put(image.bytes, registers_offset + 8 * i, frame.registers[i], 8);
        put(image.bytes, sp_offset, frame.sp, 8);
        put(image.bytes, pc_offset, frame.pc, 8);
        put(image.bytes, pstate_offset, frame.flags, 8);
        put(image.bytes, reserved_offset, fpsimd_magic, 4);
        put(image.bytes, reserved_offset + 4, fpsimd_bytes, 4);
        // Backend slot 31 is FPCR in its low word and FPSR in its high word.
        put(image.bytes, reserved_offset + 8, frame.registers[31] >> 32, 4);
        put(image.bytes, reserved_offset + 12, frame.registers[31], 4);
        for (usize i = 0; i < frame.fp_state.size(); ++i)
            image.bytes[reserved_offset + 16 + i] = static_cast<std::byte>(frame.fp_state[i]);
        put(image.bytes, frame_bytes, frame.registers[29], 8);
        put(image.bytes, frame_bytes + 8, frame.registers[30], 8);
        // Zero initialization supplies the terminator and clears all padding,
        // unused siginfo fields, fault address, uc_flags and uc_link.
        out = image;
        return true;
    }

    template <typename Read, typename StackPolicy = DisabledStack>
        requires requires(const StackPolicy &policy, Stack stack, Stack &normalized) {
            { policy(stack, normalized) } noexcept -> std::same_as<bool>;
        }
    static bool decode(u64 syscall_sp, const Frame &current, u64 begin, u64 end,
                       Read &&read, Restored &out, const StackPolicy &policy = {})
    {
        if ((syscall_sp & 15) || !range(syscall_sp, frame_bytes, begin, end)) return false;
        std::array<std::byte, frame_bytes> bytes{};
        if (!read(syscall_sp, std::span<std::byte>{bytes})) return false;
        const u64 pc = get(bytes, pc_offset, 8), sp = get(bytes, sp_offset, 8), pstate = get(bytes, pstate_offset, 8);
        if (!user_state(pc, sp, pstate, begin, end)) return false;
        Restored candidate;
        candidate.frame = current; // TPIDR_EL0/TPIDRRO_EL0 are not mcontext fields.
        candidate.stack = {get(bytes, stack_offset, 8), get(bytes, stack_offset + 16, 8),
                           static_cast<u32>(get(bytes, stack_offset + 8, 4))};
        Stack normalized;
        if (!policy(candidate.stack, normalized)) return false;
        candidate.stack = normalized;
        for (usize i = 0; i < 31; ++i) candidate.frame.registers[i] = get(bytes, registers_offset + 8 * i, 8);
        candidate.frame.pc = pc;
        candidate.frame.sp = sp;
        candidate.frame.flags = pstate;
        candidate.mask.words[0] = get(bytes, mask_offset, 8);
        candidate.mask = MaskCodec::sanitize(candidate.mask);

        usize offset = reserved_offset;
        bool have_fpsimd = false, terminated = false;
        while (frame_bytes - offset >= 8)
        {
            if (offset & 15) return false;
            const u32 magic = static_cast<u32>(get(bytes, offset, 4));
            const u32 size = static_cast<u32>(get(bytes, offset + 4, 4));
            if (!magic)
            {
                if (size) return false;
                terminated = true;
                break;
            }
            if (size < 16 || (size & 15) || size > frame_bytes - offset) return false;
            if (magic == fpsimd_magic)
            {
                if (have_fpsimd || size != fpsimd_bytes) return false;
                have_fpsimd = true;
                candidate.frame.registers[31] = get(bytes, offset + 12, 4) | (get(bytes, offset + 8, 4) << 32);
                for (usize i = 0; i < candidate.frame.fp_state.size(); ++i)
                    candidate.frame.fp_state[i] = static_cast<u8>(bytes[offset + 16 + i]);
            }
            else if (magic == esr_magic)
            {
                if (size != 16) return false; // Informational record, never restored to EL1.
            }
            else return false; // No EXTRA, SVE, SME, GCS, TPIDR2, FPMR, POE or unknown records.
            offset += size;
        }
        if (!terminated || !have_fpsimd) return false;
        out = candidate; // No live state or output changes before every check succeeds.
        return true;
    }

    static bool setup_handler(const Frame &current, Placement placement, Info info, u64 handler,
                              u64 restorer, bool siginfo, u64 begin, u64 end, Frame &out)
    {
        if ((placement.address & 15) || !range(placement.address, image_bytes, begin, end) ||
            placement.record_address != placement.address + frame_bytes ||
            !user_state(handler, placement.address, current.flags, begin, end) ||
            !range(restorer, 4, begin, end) || (restorer & 3) || !valid_info(info)) return false;
        Frame candidate = current;
        candidate.registers[0] = static_cast<u64>(info.signal);
        if (siginfo)
        {
            candidate.registers[1] = placement.address + info_offset;
            candidate.registers[2] = placement.address + ucontext_offset;
        }
        candidate.registers[29] = placement.record_address;
        candidate.registers[30] = restorer;
        candidate.pc = handler;
        candidate.sp = placement.address;
        out = candidate;
        return true;
    }
};

} // namespace ok::user::native
