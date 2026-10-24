#pragma once

#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"

namespace ok::user::native
{

enum class MipsO32SignalFormat : u8 { legacy, realtime };

// Big-endian native 32-bit o32, scalar eager FR0 only. This value codec does
// not prove hardware capability, map a VDSO, select altstack or publish state.
template <MipsO32SignalFormat Format>
struct MipsO32SignalFrame
{
    static_assert(Format == MipsO32SignalFormat::legacy || Format == MipsO32SignalFormat::realtime);
    using Frame = arch::execution::UserFrame;
    using MaskCodec = SignalMaskCodec<abi::linux_user::Profile::mips>;
    using InfoCodec = SignalInfoCodec<abi::linux_user::Profile::mips>;
    static constexpr bool realtime = Format == MipsO32SignalFormat::realtime;
    static constexpr usize frame_bytes = realtime ? 784 : 632;
    static constexpr usize info_offset = 24, uc_offset = 152;
    static constexpr usize context_offset = realtime ? 176 : 24;
    static constexpr usize mask_offset = realtime ? 768 : 616;
    static constexpr usize stack_offset = uc_offset + 8;
    static constexpr usize pc_offset = context_offset + 8, registers_offset = context_offset + 16;
    static constexpr usize fp_offset = context_offset + 272, fcsr_offset = context_offset + 532;
    static constexpr usize used_math_offset = context_offset + 540;
    static constexpr usize hi_offset = context_offset + 552, lo_offset = context_offset + 560;
    static constexpr u32 return_syscall = realtime ? 4193 : 4119;
    static constexpr u32 used_fp = 1, fcsr_reserved = 0x007c0000;
    static constexpr u32 fcsr_enable = 0x00000f80, fcsr_unimplemented = 0x00020000;
    static constexpr u64 word_max = 0xffffffffULL, user_ceiling = 0x80000000ULL;

    enum class FpContract : u8 { unverified, eager_fr0_legacy_nan_no_extensions };
    enum class ResumeContract : u8 { unverified, normalized_resume_pc };
    struct Capture
    {
        FpContract fp{FpContract::unverified};
        ResumeContract resume{ResumeContract::unverified};
    };
    struct Stack
    {
        u64 pointer{}, size{};
        u32 flags{2};
        constexpr bool operator==(const Stack &) const = default;
    };
    struct Info { i32 signal{}, code{}; u32 sender_pid{}, sender_uid{}; i32 child_status{}; };
    struct Placement { u64 address{}; constexpr bool operator==(const Placement &) const = default; };
    struct Image { std::array<std::byte, frame_bytes> bytes{}; };
    struct FcsrPatch
    {
        u64 address{};
        std::array<std::byte, 4> bytes{}; // Already encoded big-endian user bytes.
        bool required{};
        constexpr bool operator==(const FcsrPatch &) const = default;
    };
    enum class Pending : u8 { none, sigfpe };
    struct [[nodiscard]] ReturnPlan
    {
        Frame frame{};
        SignalMask mask{};
        Stack stack{};
        bool restore_altstack{realtime};
        // Linux clears enabled causes in the USER frame, then forces SIGFPE.
        // The consumer must complete this checked write before live commit and
        // arrange pending SIGFPE before reentry. This is not an ignorable note.
        FcsrPatch fcsr_patch{};
        Pending pending{Pending::none};
    };

    static constexpr bool window(u64 begin, u64 end)
    {
        return begin < end && end <= user_ceiling;
    }
    static constexpr bool range(u64 address, u64 size, u64 begin, u64 end)
    {
        return window(begin, end) && address >= begin && address < end && size <= end - address;
    }
    static constexpr bool program(u64 pc, u64 begin, u64 end)
    {
        return !(pc & 3) && range(pc, 4, begin, end);
    }
    static constexpr bool stack_pointer(u64 sp, u64 begin, u64 end)
    {
        return window(begin, end) && sp > begin && sp <= end && !(sp & 7);
    }
    static constexpr bool valid_info(Info info)
    {
        return InfoCodec::template frame_valid<true>(info);
    }
    static constexpr bool capture(const Frame &frame, Capture contract)
    {
        // FR0 and CU1 are actually captured in CP0.Status by the backend. Mode,
        // ISA, no-extension capabilities and resolved delay-slot resume PC
        // still require a trusted caller contract; unverified defaults reject.
        return contract.fp == FpContract::eager_fr0_legacy_nan_no_extensions &&
            contract.resume == ResumeContract::normalized_resume_pc &&
            frame.flags <= word_max && (frame.flags & 0x24000000u) == 0x20000000u;
    }
    // Linux subtracts its 32-byte FP-emulator gap BEFORE sigsp selects a new
    // alternate stack. A fresh alternate top has no gap; normal/nested stacks do.
    // Selection and containment, including the pre-gap sigsp boundary, are policy.
    static constexpr bool place(u64 selected_top, bool switched_to_altstack,
                                u64 begin, u64 end, Placement &out)
    {
        if (!window(begin, end) || selected_top <= begin || selected_top > end) return false;
        const u64 gap = switched_to_altstack ? 0 : 32;
        if (selected_top - begin < gap + frame_bytes) return false;
        const u64 address = (selected_top - gap - frame_bytes) & ~u64{7};
        if (!range(address, frame_bytes, begin, end)) return false;
        out = {address};
        return true;
    }
    template <usize N>
    static constexpr u64 get(const std::array<std::byte, N> &bytes, usize at, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i) value = (value << 8) | static_cast<u64>(bytes[at+i]);
        return value;
    }
    template <usize N>
    static constexpr void put(std::array<std::byte, N> &bytes, usize at, u64 value, usize count)
    {
        for (usize i = 0; i < count; ++i)
        {
            bytes[at+count-1-i] = static_cast<std::byte>(value);
            value >>= 8; // Constant shifts avoid an o32 variable-u64 libgcc helper.
        }
    }
    static u32 backend_word(const Frame &frame, usize at)
    {
        u32 value{};
        for (usize i = 0; i < 4; ++i) value = (value << 8) | frame.fp_state[at+i];
        return value;
    }
    static void set_backend_word(Frame &frame, usize at, u32 value)
    {
        for (usize i = 0; i < 4; ++i) frame.fp_state[at+i] = static_cast<u8>(value >> (8*(3-i)));
    }
    static constexpr u32 enabled_causes(u32 fcsr)
    {
        return fcsr_unimplemented | ((fcsr & fcsr_enable) << 5);
    }
    static constexpr std::array<u32, 2> restorer_words()
    {
        return {0x24020000u | return_syscall, 0x0000000c}; // addiu v0,zero,NR; syscall
    }
    [[nodiscard]] static bool encode(const Frame &frame, Capture contract, SignalMask mask,
                                    Stack stack, Info info, u64 begin, u64 end, Image &out)
    {
        const u32 fcsr = backend_word(frame, 256);
        if (!capture(frame, contract) || !program(frame.pc, begin, end) || frame.sp > word_max ||
            !valid_info(info) || stack.pointer > word_max || stack.size > word_max ||
            (fcsr & fcsr_reserved) || (fcsr & enabled_causes(fcsr))) return false;
        for (usize i = 1; i < 32; ++i)
            if (i != 26 && i != 27 && i != 29 && frame.registers[i] > word_max) return false;
        Image image;
        if constexpr (realtime)
        {
            if (!InfoCodec::encode_into(info, image.bytes, info_offset)) return false;
            put(image.bytes, stack_offset, stack.pointer, 4);
            put(image.bytes, stack_offset+4, stack.size, 4); // MIPS stack_t is sp,size,flags.
            put(image.bytes, stack_offset+8, stack.flags, 4);
        }
        put(image.bytes, pc_offset, frame.pc, 8);
        for (usize i = 1; i < 32; ++i)
            if (i != 26 && i != 27)
                put(image.bytes, registers_offset+8*i, i == 29 ? frame.sp : frame.registers[i], 8);
        // Native 32-bit unsigned-long values zero-extend to Linux's u64 slots.
        // k0/k1 are reserved and not retained by either ordinary Linux or backend.
        put(image.bytes, hi_offset, backend_word(frame, 272), 8);
        put(image.bytes, lo_offset, backend_word(frame, 276), 8);
        for (usize reg = 0; reg < 32; reg += 2)
            for (usize byte = 0; byte < 8; ++byte)
                image.bytes[fp_offset+reg*8+byte] = static_cast<std::byte>(frame.fp_state[reg*8+byte]);
        put(image.bytes, fcsr_offset, fcsr, 4);
        put(image.bytes, used_math_offset, used_fp, 4);
        const auto encoded_mask = MaskCodec::encode(MaskCodec::sanitize(mask));
        for (usize i = 0; i < encoded_mask.size(); ++i) image.bytes[mask_offset+i] = encoded_mask[i];
        // Argument-save space, old trampoline padding, odd FR0 slots, absent
        // DSP/SmartMIPS fields and unused metadata have deterministic zero bytes.
        out = image;
        return true;
    }
    [[nodiscard]] static bool setup_handler(const Frame &current, Capture contract, Placement placement,
                                           Info info, u64 handler, u64 restorer,
                                           u64 begin, u64 end, Frame &out)
    {
        const u32 fcsr = backend_word(current, 256);
        if (!capture(current, contract) || (fcsr & fcsr_reserved) || (fcsr & enabled_causes(fcsr)) ||
            !valid_info(info) || !program(handler, begin, end) ||
            !program(restorer, begin, end) || !range(restorer, 8, begin, end) ||
            (placement.address & 7) || !range(placement.address, frame_bytes, begin, end)) return false;
        Frame candidate = current;
        candidate.pc = candidate.registers[25] = static_cast<uptr>(handler);
        candidate.sp = candidate.registers[29] = static_cast<uptr>(placement.address);
        candidate.registers[0] = 0;
        candidate.registers[4] = static_cast<uptr>(info.signal);
        candidate.registers[5] = realtime ? static_cast<uptr>(placement.address+info_offset) : 0;
        candidate.registers[6] = static_cast<uptr>(placement.address+(realtime ? uc_offset : context_offset));
        candidate.registers[31] = static_cast<uptr>(restorer);
        out = candidate; // Handler inherits scalar FP state, trusted TLS and CP0 status.
        return true;
    }
    template <typename Read>
    [[nodiscard]] static bool decode(u64 syscall_sp, const Frame &current, Capture contract,
                                    u64 begin, u64 end, Read &&read, ReturnPlan &out)
    {
        if (!capture(current, contract) || (syscall_sp & 7) || !range(syscall_sp, frame_bytes, begin, end)) return false;
        Image image;
        if (!read(syscall_sp, std::span<std::byte>{image.bytes})) return false;
        const auto &bytes = image.bytes;
        // Native 32-bit Linux truncates u64 context fields to unsigned long.
        const u32 pc = static_cast<u32>(get(bytes, pc_offset, 8));
        const u32 sp = static_cast<u32>(get(bytes, registers_offset+8*29, 8));
        if (!program(pc, begin, end) || !stack_pointer(sp, begin, end) ||
            get(bytes, used_math_offset, 4) != used_fp) return false;
        if (get(bytes, context_offset+528, 4) || get(bytes, context_offset+544, 4)) return false;
        for (usize at = context_offset+568; at < context_offset+592; at += 4)
            if (get(bytes, at, 4)) return false; // No DSP accumulators or SmartMIPS ACX.
        const u32 fcsr = static_cast<u32>(get(bytes, fcsr_offset, 4));
        if (fcsr & fcsr_reserved) return false;
        ReturnPlan candidate;
        candidate.frame = current;
        for (usize i = 1; i < 32; ++i)
            if (i != 26 && i != 27)
                candidate.frame.registers[i] = static_cast<uptr>(static_cast<u32>(get(bytes, registers_offset+8*i, 8)));
        candidate.frame.registers[0] = 0;
        candidate.frame.pc = pc;
        candidate.frame.sp = candidate.frame.registers[29] = sp;
        set_backend_word(candidate.frame, 272, static_cast<u32>(get(bytes, hi_offset, 8)));
        set_backend_word(candidate.frame, 276, static_cast<u32>(get(bytes, lo_offset, 8)));
        for (usize reg = 0; reg < 32; reg += 2)
            for (usize byte = 0; byte < 8; ++byte)
                candidate.frame.fp_state[reg*8+byte] = static_cast<u8>(bytes[fp_offset+reg*8+byte]);
        const u32 enabled = enabled_causes(fcsr);
        const bool pending = fcsr & enabled;
        const u32 normalized = pending ? fcsr & ~enabled : fcsr;
        set_backend_word(candidate.frame, 256, normalized);
        if (pending)
        {
            candidate.fcsr_patch.address = syscall_sp+fcsr_offset;
            put(candidate.fcsr_patch.bytes, 0, normalized, 4);
            candidate.fcsr_patch.required = true;
            candidate.pending = Pending::sigfpe;
        }
        std::array<std::byte, MaskCodec::bytes> mask{};
        for (usize i = 0; i < mask.size(); ++i) mask[i] = bytes[mask_offset+i];
        candidate.mask = MaskCodec::sanitize(MaskCodec::decode(mask));
        if constexpr (realtime)
            candidate.stack = {get(bytes, stack_offset, 4), get(bytes, stack_offset+4, 4),
                               static_cast<u32>(get(bytes, stack_offset+8, 4))};
        // sc_status/regmask/eir cannot change privilege state. TLS has no wire
        // field and remains trusted. Reserved k0/k1 and unused FP bytes survive.
        out = candidate;
        return true;
    }
    template <typename Write>
    [[nodiscard]] static bool prepare(const Frame &current, Capture contract, SignalMask mask,
                                     Stack stack, Info info, u64 selected_top, bool switched_to_altstack,
                                     u64 handler, u64 restorer, u64 begin, u64 end, Write &&write,
                                     Placement &placement_out, Frame &frame_out)
    {
        Placement placement;
        Image image;
        Frame handler_frame;
        if (!place(selected_top, switched_to_altstack, begin, end, placement) ||
            !encode(current, contract, mask, stack, info, begin, end, image) ||
            !setup_handler(current, contract, placement, info, handler, restorer, begin, end, handler_frame) ||
            !write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        placement_out = placement;
        frame_out = handler_frame;
        return true;
    }
};
using MipsO32LegacySignalFrame = MipsO32SignalFrame<MipsO32SignalFormat::legacy>;
using MipsO32RtSignalFrame = MipsO32SignalFrame<MipsO32SignalFormat::realtime>;
} // namespace ok::user::native
