#pragma once

#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"

namespace ok::user::native
{

// Linux x86_64 RT layout for the current FXSAVE-only native CPU contract.
// Private Debug delivery uses this codec; the production interface is separate.
struct X86SignalFrame
{
    using Frame = arch::execution::UserFrame;
    using MaskCodec = SignalMaskCodec<abi::linux_user::Profile::x86_64>;
    using InfoCodec = SignalInfoCodec<abi::linux_user::Profile::x86_64>;
    static constexpr usize prefix_bytes = 440, restore_bytes = 312, fp_bytes = 512;
    static constexpr usize context_offset = 48, mask_offset = 304, info_offset = 312;
    static_assert(info_offset + InfoCodec::bytes <= prefix_bytes);
    static constexpr u64 user_cs = 0x23, user_ss = 0x1b;
    static constexpr u64 flags_mask = 0x250fd5;
    static constexpr u64 restored_flags_mask = 0x50dd5; // Linux FIX_EFLAGS.
    // Linux register order mapped to this backend's assembly capture order.
    static constexpr std::array<usize, 15> register_slots{7, 6, 5, 4, 3, 2, 1, 0, 9, 10, 8, 13, 11, 14, 12};

    struct Stack { u64 pointer{}; u64 size{}; u32 flags{2}; };
    // Typed sender/child payload. Other fault and timer variants remain unavailable.
    struct Info { i32 signal{}; i32 code{}; u32 sender_pid{}; u32 sender_uid{}; i32 child_status{}; };
    static constexpr bool valid_info(Info info)
    {
        return InfoCodec::template frame_valid<>(info);
    }
    struct Placement { u64 address{}; u64 fp_address{}; };
    struct Image
    {
        std::array<std::byte, prefix_bytes> prefix{};
        std::array<std::byte, fp_bytes> fp{};
    };
    struct Restored { Frame frame{}; SignalMask mask{}; Stack stack{}; };

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
    static constexpr bool range(u64 address, u64 size, u64 begin, u64 end)
    {
        return begin <= address && address < end && size <= end - address;
    }
    static constexpr bool place_below(u64 top, u64 begin, u64 end, Placement &out)
    {
        // Top has already accounted for red-zone/alternate-stack selection.
        if (top <= begin || top > end || top - begin < fp_bytes) return false;
        const u64 fp = (top - fp_bytes) & ~u64{63};
        if (fp < begin || fp - begin < prefix_bytes) return false;
        const u64 aligned = (fp - prefix_bytes) & ~u64{15};
        if (aligned < 8) return false;
        const u64 frame = aligned - 8;
        if (!range(frame, prefix_bytes, begin, end) || !range(fp, fp_bytes, begin, end)) return false;
        out = {frame, fp};
        return true;
    }
    static constexpr bool place(u64 top, u64 begin, u64 end, Placement &out)
    {
        return top <= end && top >= 128 && place_below(top - 128, begin, end, out);
    }
    static constexpr std::array<std::byte, fp_bytes> reset_fp()
    {
        std::array<std::byte, fp_bytes> fp{};
        put(fp, 0, 0x37f, 2);
        put(fp, 24, 0x1f80, 4);
        return fp;
    }
    static constexpr void scrub_fp(std::array<std::byte, fp_bytes> &fp, u32 mxcsr_mask)
    {
        fp[5] = std::byte{};
        put(fp, 28, mxcsr_mask, 4);
        for (usize i = 0; i < 8; ++i)
            for (usize j = 10; j < 16; ++j) fp[32 + 16 * i + j] = std::byte{};
        // FXSAVE leaves these bytes untouched. Never export stale storage.
        for (usize i = 416; i < fp.size(); ++i) fp[i] = std::byte{};
    }
    static Image encode(const Frame &frame, SignalMask mask, Stack stack, Info info,
                        Placement placement, u64 restorer, u32 mxcsr_mask)
    {
        // The delivery bridge validates before any user copy. Keep the existing
        // value-returning API and return an empty image for invalid direct input.
        if (!valid_info(info)) return {};
        Image image;
        put(image.prefix, 0, restorer, 8);
        put(image.prefix, 8, 6, 8); // UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS; no XSAVE.
        put(image.prefix, 24, stack.pointer, 8);
        put(image.prefix, 32, stack.flags, 4);
        put(image.prefix, 40, stack.size, 8);
        for (usize i = 0; i < register_slots.size(); ++i)
            put(image.prefix, context_offset + i * 8, frame.registers[register_slots[i]], 8);
        put(image.prefix, context_offset + 120, frame.sp, 8);
        put(image.prefix, context_offset + 128, frame.pc, 8);
        put(image.prefix, context_offset + 136, frame.flags, 8);
        put(image.prefix, context_offset + 144, user_cs, 2);
        put(image.prefix, context_offset + 150, user_ss, 2);
        put(image.prefix, context_offset + 168, mask.words[0], 8);
        put(image.prefix, context_offset + 184, placement.fp_address, 8);
        put(image.prefix, mask_offset, mask.words[0], 8);
        if (!InfoCodec::encode_into(info, image.prefix, info_offset)) return {};
        for (usize i = 0; i < fp_bytes; ++i) image.fp[i] = static_cast<std::byte>(frame.fp_state[i]);
        scrub_fp(image.fp, mxcsr_mask ? mxcsr_mask : 0xffbf);
        return image;
    }

    // Caller supplies checked copies and a trusted CPU MXCSR mask. Decode into
    // staging only; any failure must become a user fault, never ordinary EFAULT
    // returned into a restorer, and must not partially replace live registers.
    struct DisabledStack
    {
        constexpr bool operator()(Stack raw, Stack &normalized) const noexcept
        {
            if (raw.flags != 2) return false;
            normalized = {};
            return true;
        }
    };
    template <typename Read, typename StackPolicy = DisabledStack>
    static bool decode(u64 syscall_sp, const Frame &current, u64 begin, u64 end,
                       u32 mxcsr_mask, Read &&read, Restored &out, const StackPolicy &policy = {})
    {
        if (syscall_sp < 8 || !range(syscall_sp - 8, prefix_bytes, begin, end)) return false;
        std::array<std::byte, restore_bytes> bytes{};
        if (!read(syscall_sp - 8, std::span<std::byte>{bytes})) return false;
        const u64 uc_flags = get(bytes, 8, 8);
        // XSAVE, compatibility selectors and arbitrary segments are unavailable.
        if (uc_flags & ~u64{6}) return false;
        const u64 cs = get(bytes, context_offset + 144, 2), ss = get(bytes, context_offset + 150, 2);
        if (cs != user_cs || ((uc_flags & 4) && ss != user_ss)) return false;
        const u64 pc = get(bytes, context_offset + 128, 8), sp = get(bytes, context_offset + 120, 8);
        if (!range(pc, 1, begin, end) || sp <= begin || sp > end) return false;
        Restored candidate;
        candidate.frame = current; // FS/GS bases are not fields in Linux mcontext.
        for (usize i = 0; i < register_slots.size(); ++i)
            candidate.frame.registers[register_slots[i]] = get(bytes, context_offset + i * 8, 8);
        candidate.frame.pc = pc;
        candidate.frame.sp = sp;
        candidate.frame.flags = (((current.flags & ~restored_flags_mask) |
            (get(bytes, context_offset + 136, 8) & restored_flags_mask)) & flags_mask) | 0x202;
        candidate.mask.words[0] = get(bytes, mask_offset, 8);
        candidate.mask = MaskCodec::sanitize(candidate.mask);
        candidate.stack = {get(bytes, 24, 8), get(bytes, 40, 8), static_cast<u32>(get(bytes, 32, 4))};
        Stack normalized;
        if (!policy(candidate.stack, normalized)) return false;
        candidate.stack = normalized;
        const u64 fp_address = get(bytes, context_offset + 184, 8);
        auto fp = reset_fp();
        if (fp_address)
        {
            if (!range(fp_address, fp_bytes, begin, end) ||
                !read(fp_address, std::span<std::byte>{fp})) return false;
            if (get(fp, 464, 4) != 0) return false; // No extended xstate records.
            const u32 permitted = mxcsr_mask ? mxcsr_mask : 0xffbf;
            if (get(fp, 24, 4) & ~u64{permitted}) return false;
            scrub_fp(fp, permitted);
        }
        for (usize i = 0; i < fp_bytes; ++i) candidate.frame.fp_state[i] = static_cast<u8>(fp[i]);
        out = candidate;
        return true;
    }
};

} // namespace ok::user::native
