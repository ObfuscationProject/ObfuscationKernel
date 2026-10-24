#pragma once

#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"

namespace ok::user::native
{

struct I386SignalWire
{
    template <typename T, usize N>
    static constexpr u64 get(const std::array<T, N> &bytes, usize offset, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i) value |= static_cast<u64>(bytes[offset + i]) << (8 * i);
        return value;
    }
    template <typename T, usize N>
    static constexpr void put(std::array<T, N> &bytes, usize offset, u64 value, usize count)
    {
        for (usize i = 0; i < count; ++i) bytes[offset + i] = static_cast<T>(value >> (8 * i));
    }
};

// Native i386 FXSAVE only: legacy x87 state is authoritative, XMM0-XMM7.
// The MXCSR mask is trusted policy, never the mask supplied by a user frame.
struct I386SignalFp : I386SignalWire
{
    static constexpr usize prefix_bytes = 112, fx_bytes = 512, bytes = 624;
    static constexpr u32 mxcsr_mask = 0xffbf, xstate_magic = 0x46505853;
    using Private = std::array<u8, fx_bytes>;
    using Image = std::array<std::byte, bytes>;

    static constexpr Private initial()
    {
        Private state{};
        put(state, 0, 0x037f, 2);
        put(state, 24, 0x1f80, 4);
        put(state, 28, mxcsr_mask, 4);
        return state;
    }
    static constexpr u32 full_tag(const Private &fx)
    {
        const usize top = (get(fx, 2, 2) >> 11) & 7;
        const u32 abridged = static_cast<u32>(get(fx, 4, 1));
        u32 out = 0xffff0000;
        for (usize physical = 0; physical < 8; ++physical)
        {
            u32 tag = 3;
            if (abridged & (1U << physical))
            {
                const usize st = 32 + ((physical + 8 - top) & 7) * 16;
                const auto exponent = get(fx, st + 8, 2) & 0x7fff;
                const auto significand = get(fx, st, 8);
                tag = exponent == 0x7fff ? 2 : exponent == 0 ? (significand ? 2 : 1) :
                      (significand & (u64{1} << 63)) ? 0 : 2;
            }
            out |= tag << (2 * physical);
        }
        return out;
    }
    static constexpr u8 abridged_tag(u16 full)
    {
        u8 out{};
        for (usize i = 0; i < 8; ++i)
            if (((full >> (2 * i)) & 3) != 3) out |= static_cast<u8>(1U << i);
        return out;
    }
    static bool encode(const Private &fx, Image &out)
    {
        const u32 cw = static_cast<u32>(get(fx, 0, 2)), fop = static_cast<u32>(get(fx, 6, 2));
        const u32 mxcsr = static_cast<u32>(get(fx, 24, 4));
        if ((cw & ~0x1f7fU) || (fop & ~0x7ffU) || (mxcsr & ~mxcsr_mask)) return false;
        Image candidate{};
        put(candidate, 0, cw | 0xffff0000U, 4);
        put(candidate, 4, get(fx, 2, 2) | 0xffff0000U, 4);
        put(candidate, 8, full_tag(fx), 4);
        put(candidate, 12, get(fx, 8, 4), 4);
        put(candidate, 16, get(fx, 12, 2) | (u64{fop} << 16), 4);
        put(candidate, 20, get(fx, 16, 4), 4);
        put(candidate, 24, get(fx, 20, 2), 4);
        put(candidate, 108, get(fx, 2, 2), 2); // status; magic at 110 stays FXSR zero.
        // Copy only architectural fields; initialize every reserved byte.
        for (usize i = 0; i < 5; ++i) candidate[112 + i] = static_cast<std::byte>(fx[i]);
        put(candidate, 118, fop, 2);
        put(candidate, 120, get(fx, 8, 4), 4);
        put(candidate, 124, get(fx, 12, 2), 2);
        put(candidate, 128, get(fx, 16, 4), 4);
        put(candidate, 132, get(fx, 20, 2), 2);
        put(candidate, 136, mxcsr, 4);
        put(candidate, 140, mxcsr_mask, 4);
        for (usize reg = 0; reg < 8; ++reg)
            for (usize byte = 0; byte < 10; ++byte)
            {
                candidate[28 + reg * 10 + byte] = static_cast<std::byte>(fx[32 + reg * 16 + byte]);
                candidate[144 + reg * 16 + byte] = static_cast<std::byte>(fx[32 + reg * 16 + byte]);
            }
        for (usize i = 0; i < 128; ++i) candidate[272 + i] = static_cast<std::byte>(fx[160 + i]);
        // Linux writes this FX-only descriptor even without XSAVE; no magic2
        // trailer exists. xstate_size 512 cannot describe a valid XSAVE header.
        put(candidate, 576, xstate_magic, 4);
        put(candidate, 580, 628, 4);
        put(candidate, 584, 3, 8);
        put(candidate, 592, 512, 4);
        out = candidate;
        return true;
    }
    static bool decode(const Image &image, Private &out)
    {
        const u32 cw = static_cast<u32>(get(image, 0, 2));
        const u32 fop = static_cast<u32>(get(image, 16, 4) >> 16);
        const u32 mxcsr = static_cast<u32>(get(image, 136, 4));
        if (get(image, 110, 2) || (cw & ~0x1f7fU) || (fop & ~0x7ffU) || (mxcsr & ~mxcsr_mask)) return false;
        const u32 magic = static_cast<u32>(get(image, 576, 4));
        if (magic == xstate_magic)
        {
            if (get(image, 580, 4) != 628 || get(image, 584, 8) != 3 || get(image, 592, 4) != 512) return false;
        }
        else if (magic || get(image, 580, 4) || get(image, 584, 8) || get(image, 592, 4)) return false;
        for (usize i = 596; i < bytes; ++i) if (image[i] != std::byte{}) return false;
        Private candidate{};
        put(candidate, 0, cw, 2);
        put(candidate, 2, get(image, 4, 2), 2);
        put(candidate, 4, abridged_tag(static_cast<u16>(get(image, 8, 2))), 1);
        put(candidate, 6, fop, 2);
        put(candidate, 8, get(image, 12, 4), 4);
        put(candidate, 12, get(image, 16, 2), 2);
        put(candidate, 16, get(image, 20, 4), 4);
        put(candidate, 20, get(image, 24, 2), 2);
        put(candidate, 24, mxcsr, 4);
        put(candidate, 28, mxcsr_mask, 4);
        for (usize reg = 0; reg < 8; ++reg)
            for (usize byte = 0; byte < 10; ++byte)
                candidate[32 + reg * 16 + byte] = static_cast<u8>(image[28 + reg * 10 + byte]);
        for (usize i = 0; i < 128; ++i) candidate[160 + i] = static_cast<u8>(image[272 + i]);
        out = candidate;
        return true;
    }
};

enum class I386SignalKind : u8 { legacy, realtime };

template <I386SignalKind Kind>
    requires (Kind == I386SignalKind::legacy || Kind == I386SignalKind::realtime)
struct I386SignalFrame : I386SignalWire
{
    using Frame = arch::execution::UserFrame;
    using MaskCodec = SignalMaskCodec<abi::linux_user::Profile::i386>;
    using InfoCodec = SignalInfoCodec<abi::linux_user::Profile::i386>;
    static constexpr bool realtime = Kind == I386SignalKind::realtime;
    static constexpr usize prefix_bytes = realtime ? 268 : 732, context_offset = realtime ? 164 : 8;
    static constexpr usize retcode_offset = realtime ? 260 : 724, syscall_sp_delta = realtime ? 4 : 8;
    static constexpr usize max_image_bytes = prefix_bytes + 15 + I386SignalFp::bytes;
    static constexpr u32 flags_mask = 0x250fd5, restored_flags_mask = 0x50dd5;
    static constexpr u16 user_cs = 0x23, user_ds = 0x1b;
    static constexpr u64 word_mask = 0xffffffffULL;
    struct Stack { u64 base{}, size{}; u32 flags{2}; };
    struct Info { i32 signal{}, code{}; u32 sender_pid{}, sender_uid{}; i32 child_status{}; };
    struct TrapInfo { u32 number{}, error{}, address{}; };
    struct Placement { u64 address{}, fp_address{}, fx_address{}; };
    struct Image { std::array<std::byte, max_image_bytes> bytes{}; usize used{}; };
    struct Restored { Frame frame{}; SignalMask mask{}; Stack stack{}; static constexpr bool restores_stack = realtime; };

    static constexpr bool range(u64 address, u64 size, u64 begin, u64 end)
    {
        return begin < end && end <= word_mask && begin <= address && address < end && size <= end - address;
    }
    static constexpr bool data_selector(u64 value)
    {
        return value <= 3 || value == user_ds || value == user_cs || value == 0x33 || value == 0x3b;
    }
    static constexpr bool stack_selector(u64 value) { return value == user_ds || value == 0x33 || value == 0x3b; }
    static constexpr bool tls_base(u64 base, u64 begin, u64 end) { return !base || range(base, 1, begin, end); }
    static constexpr bool valid_info(Info info)
    {
        return InfoCodec::template frame_valid<>(info);
    }
    static constexpr bool valid_placement(Placement p, u64 begin, u64 end)
    {
        return range(p.address, prefix_bytes, begin, end) && (p.address & 15) == 12 &&
               range(p.fp_address, I386SignalFp::bytes, begin, end) && p.fp_address + 112 == p.fx_address &&
               !(p.fx_address & 63) && p.fp_address >= p.address + prefix_bytes &&
               p.fp_address - p.address - prefix_bytes < 16;
    }
    static constexpr bool place(u64 top, u64 begin, u64 end, Placement &out)
    {
        if (begin >= end || end > word_mask || top <= begin || top > end || top - begin < 512) return false;
        const u64 fx = (top - 512) & ~u64{63};
        if (fx < begin || fx - begin < 112 + prefix_bytes) return false;
        const u64 fp = fx - 112, unaligned = fp - prefix_bytes;
        if (unaligned < 12) return false;
        const Placement candidate{((unaligned - 12) & ~u64{15}) + 12, fp, fx};
        if (!valid_placement(candidate, begin, end)) return false;
        out = candidate;
        return true;
    }
    static constexpr bool select_restorer(bool action_selected, u64 action, u64 fallback,
                                          u64 begin, u64 end, u64 &out)
    {
        const u64 chosen = action_selected ? action : fallback;
        if (!range(chosen, 1, begin, end)) return false;
        out = chosen; // Caller validates mapping/bytes and the legacy/RT kind.
        return true;
    }
    static bool encode(const Frame &frame, SignalMask mask, Stack stack, Info info, TrapInfo trap,
                       Placement placement, u64 restorer, u64 begin, u64 end, Image &out)
    {
        if (!valid_placement(placement, begin, end) || !range(restorer, 1, begin, end) || !valid_info(info) ||
            frame.pc > word_mask || frame.sp > word_mask || frame.flags > word_mask) return false;
        for (usize i = 0; i < 8; ++i) if (i != 3 && frame.registers[i] > word_mask) return false;
        if constexpr (realtime) if (stack.base > word_mask || stack.size > word_mask) return false;
        I386SignalFp::Image fp;
        if (!I386SignalFp::encode(frame.fp_state, fp)) return false;
        Image candidate;
        candidate.used = static_cast<usize>(placement.fp_address - placement.address + fp.size());
        put(candidate.bytes, 0, restorer, 4);
        put(candidate.bytes, 4, static_cast<u32>(info.signal), 4);
        const auto encoded_mask = MaskCodec::encode(mask);
        if constexpr (realtime)
        {
            put(candidate.bytes, 8, placement.address + 16, 4);
            put(candidate.bytes, 12, placement.address + 144, 4);
            if (!InfoCodec::encode_into(info, candidate.bytes, 16)) return false;
            put(candidate.bytes, 152, stack.base, 4);
            put(candidate.bytes, 156, stack.flags, 4);
            put(candidate.bytes, 160, stack.size, 4);
            for (usize i = 0; i < 8; ++i) candidate.bytes[252 + i] = encoded_mask[i];
        }
        else for (usize i = 0; i < 4; ++i) candidate.bytes[720 + i] = encoded_mask[4 + i];
        for (usize i = 0; i < 4; ++i) put(candidate.bytes, context_offset + i * 4, frame.registers[8 + i] & 0xffff, 2);
        for (usize i = 0; i < 8; ++i)
            put(candidate.bytes, context_offset + 16 + i * 4, i == 3 ? frame.sp : frame.registers[i], 4);
        put(candidate.bytes, context_offset + 48, trap.number, 4);
        put(candidate.bytes, context_offset + 52, trap.error, 4);
        put(candidate.bytes, context_offset + 56, frame.pc, 4);
        put(candidate.bytes, context_offset + 60, user_cs, 2);
        put(candidate.bytes, context_offset + 64, frame.flags, 4);
        put(candidate.bytes, context_offset + 68, frame.sp, 4);
        put(candidate.bytes, context_offset + 72, frame.registers[12] & 0xffff, 2);
        put(candidate.bytes, context_offset + 76, placement.fp_address, 4);
        for (usize i = 0; i < 4; ++i) candidate.bytes[context_offset + 80 + i] = encoded_mask[i];
        put(candidate.bytes, context_offset + 84, trap.address, 4);
        constexpr auto operation = realtime ? abi::linux_user::Operation::rt_sigreturn : abi::linux_user::Operation::sigreturn;
        constexpr u32 number = *abi::linux_user::Codec<abi::linux_user::Profile::i386>::syscall_number(operation);
        const usize mov = retcode_offset + (realtime ? 0 : 1);
        if constexpr (!realtime) candidate.bytes[retcode_offset] = std::byte{0x58};
        candidate.bytes[mov] = std::byte{0xb8};
        put(candidate.bytes, mov + 1, number, 4);
        candidate.bytes[mov + 5] = std::byte{0xcd};
        candidate.bytes[mov + 6] = std::byte{0x80};
        const usize fp_offset = static_cast<usize>(placement.fp_address - placement.address);
        for (usize i = 0; i < fp.size(); ++i) candidate.bytes[fp_offset + i] = fp[i];
        out = candidate;
        return true;
    }
    static bool stage_handler(const Frame &interrupted, Placement p, u64 handler, Info info,
                              u64 begin, u64 end, Frame &out)
    {
        if (!valid_placement(p, begin, end) || !range(handler, 1, begin, end) || !valid_info(info) ||
            !data_selector(interrupted.registers[8] & 0xffff) || !data_selector(interrupted.registers[9] & 0xffff) ||
            !tls_base(interrupted.fs_base, begin, end) || !tls_base(interrupted.gs_base, begin, end)) return false;
        Frame candidate = interrupted;
        candidate.pc = static_cast<uptr>(handler);
        candidate.sp = static_cast<uptr>(p.address);
        candidate.registers[7] = static_cast<uptr>(info.signal);
        candidate.registers[5] = static_cast<uptr>(realtime ? p.address + 16 : 0);
        candidate.registers[6] = static_cast<uptr>(realtime ? p.address + 144 : 0);
        candidate.registers[10] = candidate.registers[11] = candidate.registers[12] = user_ds;
        candidate.flags = ((interrupted.flags & flags_mask) & ~uptr{0x10500}) | 0x202;
        candidate.fp_state = I386SignalFp::initial();
        out = candidate;
        return true;
    }
    template <typename Read>
    static bool decode(u64 syscall_sp, const Frame &current, u64 begin, u64 end, Read &&read, Restored &out)
    {
        if (syscall_sp < syscall_sp_delta) return false;
        const u64 address = syscall_sp - syscall_sp_delta;
        if (!range(address, prefix_bytes, begin, end)) return false;
        std::array<std::byte, prefix_bytes> bytes{};
        if (!read(address, std::span<std::byte>{bytes})) return false;
        if constexpr (realtime) if (get(bytes, 144, 4)) return false; // No XSAVE/unknown UC flags.
        const u64 pc = get(bytes, context_offset + 56, 4), sp = get(bytes, context_offset + 28, 4);
        const u64 ss = get(bytes, context_offset + 72, 2), cs = get(bytes, context_offset + 60, 2);
        const u32 flags = static_cast<u32>(get(bytes, context_offset + 64, 4));
        if (cs != user_cs || !stack_selector(ss) || !range(pc, 1, begin, end) ||
            (flags & ~(flags_mask | 0x202U)) || !tls_base(current.fs_base, begin, end) ||
            !tls_base(current.gs_base, begin, end)) return false;
        for (usize i = 0; i < 4; ++i) if (!data_selector(get(bytes, context_offset + 4 * i, 2))) return false;
        const u64 base = ss == 0x33 ? current.fs_base : ss == 0x3b ? current.gs_base : 0;
        if (sp > word_mask - base || sp + base <= begin || sp + base > end) return false;
        Restored candidate;
        candidate.frame = current;
        for (usize i = 0; i < 8; ++i)
            if (i != 3) candidate.frame.registers[i] = static_cast<uptr>(get(bytes, context_offset + 16 + i * 4, 4));
        for (usize i = 0; i < 4; ++i)
            candidate.frame.registers[8 + i] = static_cast<uptr>(get(bytes, context_offset + 4 * i, 2));
        candidate.frame.registers[12] = static_cast<uptr>(ss);
        candidate.frame.pc = static_cast<uptr>(pc);
        candidate.frame.sp = static_cast<uptr>(sp);
        candidate.frame.flags = (((current.flags & ~uptr{restored_flags_mask}) |
                                 (flags & restored_flags_mask)) & flags_mask) | 0x202;
        const u64 fp_address = get(bytes, context_offset + 76, 4);
        if (fp_address)
        {
            if (!range(fp_address, I386SignalFp::bytes, begin, end)) return false;
            I386SignalFp::Image fp;
            if (!read(fp_address, std::span<std::byte>{fp}) || !I386SignalFp::decode(fp, candidate.frame.fp_state)) return false;
        }
        else candidate.frame.fp_state = I386SignalFp::initial();
        std::array<std::byte, MaskCodec::bytes> mask{};
        if constexpr (realtime)
        {
            for (usize i = 0; i < 8; ++i) mask[i] = bytes[252 + i];
            candidate.stack = {get(bytes, 152, 4), get(bytes, 160, 4), static_cast<u32>(get(bytes, 156, 4))};
        }
        else for (usize i = 0; i < 4; ++i) { mask[i] = bytes[context_offset + 80 + i]; mask[4 + i] = bytes[720 + i]; }
        candidate.mask = MaskCodec::sanitize(MaskCodec::decode(mask));
        out = candidate;
        return true;
    }
};

using I386LegacySignalFrame = I386SignalFrame<I386SignalKind::legacy>;
using I386RtSignalFrame = I386SignalFrame<I386SignalKind::realtime>;

} // namespace ok::user::native
