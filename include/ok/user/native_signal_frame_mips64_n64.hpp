#pragma once

#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"

namespace ok::user::native
{

// Native big-endian n64, eager scalar FR1. Capability and resume attestations
// are trusted runtime inputs; this codec neither discovers them nor commits state.
struct Mips64N64SignalFrame
{
    using Frame = arch::execution::UserFrame;
    using MaskCodec = SignalMaskCodec<abi::linux_user::Profile::mips64>;
    using InfoCodec = SignalInfoCodec<abi::linux_user::Profile::mips64>;
    static constexpr usize frame_bytes = 808, frame_alignment = 16;
    static constexpr usize info_offset = 24, uc_offset = 152, stack_offset = 168;
    static constexpr usize context_offset = 192, fp_offset = 448, mask_offset = 792;
    static constexpr usize hi_offset = 704, lo_offset = 736, pc_offset = 768;
    static constexpr usize fcsr_offset = 776, used_math_offset = 780;
    static constexpr u32 return_syscall = 5211, used_fp_fr1 = 3;
    static constexpr u32 fcsr_reserved = 0x007c0000, fcsr_enable = 0x00000f80;
    static constexpr u64 user_ceiling = 0x80000000ULL;

    enum class FpContract : u8 { unverified, eager_fr1_legacy_nan_no_extensions };
    enum class ResumeContract : u8 { unverified, normalized_resume_pc, prepared_syscall_restart };
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
        std::array<std::byte, 4> bytes{};
        bool required{};
        constexpr bool operator==(const FcsrPatch &) const = default;
    };
    enum class Pending : u8 { none, sigfpe };
    struct [[nodiscard]] ReturnPlan
    {
        Frame frame{};
        SignalMask mask{};
        Stack stack{};
        bool restore_altstack{true};
        FcsrPatch fcsr_patch{};
        Pending pending{Pending::none};
    };

    static constexpr bool window(u64 begin, u64 end)
    { return begin < end && end <= user_ceiling; }
    static constexpr bool range(u64 address, u64 size, u64 begin, u64 end)
    { return window(begin, end) && address >= begin && address < end && size <= end-address; }
    static constexpr bool program(u64 pc, u64 begin, u64 end)
    { return !(pc & 3) && range(pc, 4, begin, end); }
    static constexpr bool stack_pointer(u64 sp, u64 begin, u64 end)
    {
        // The bound backend accepts eight-byte restored SP alignment. Linux
        // n64 signal-frame placement separately requires sixteen-byte alignment.
        return window(begin, end) && sp > begin && sp <= end && !(sp & 7);
    }
    static constexpr bool capture(const Frame &frame, Capture contract)
    {
        return contract.fp == FpContract::eager_fr1_legacy_nan_no_extensions &&
            (contract.resume == ResumeContract::normalized_resume_pc ||
             contract.resume == ResumeContract::prepared_syscall_restart) &&
            (frame.flags & 0x24000000u) == 0x24000000u;
    }
    static constexpr bool valid_info(Info info)
    {
        return InfoCodec::template frame_valid<true>(info);
    }
    static constexpr u32 enabled_causes(u32 fcsr)
    { return 0x00020000u | ((fcsr & fcsr_enable) << 5); }
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
            value >>= 8;
        }
    }
    static u64 backend_word(const Frame &frame, usize at, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i) value = (value << 8) | frame.fp_state[at+i];
        return value;
    }
    static void set_backend_word(Frame &frame, usize at, u64 value, usize count)
    {
        for (usize i = 0; i < count; ++i)
        {
            frame.fp_state[at+count-1-i] = static_cast<u8>(value);
            value >>= 8;
        }
    }
    static constexpr std::array<u32, 2> restorer_words()
    { return {0x2402145b, 0x0000000c}; }
    static constexpr bool place(u64 selected_top, bool switched_to_altstack,
                                u64 begin, u64 end, Placement &out)
    {
        if (!window(begin, end) || selected_top <= begin || selected_top > end) return false;
        // Linux subtracts the emulator gap before selecting a new altstack.
        const u64 gap = switched_to_altstack ? 0 : 32;
        if (selected_top-begin < frame_bytes+gap) return false;
        const u64 address = (selected_top-gap-frame_bytes) & ~u64{15};
        if (!range(address, frame_bytes, begin, end)) return false;
        out = {address};
        return true;
    }
    [[nodiscard]] static bool encode(const Frame &frame, Capture contract, SignalMask mask,
                                    Stack stack, Info info, u64 begin, u64 end, Image &out)
    {
        const u32 fcsr = static_cast<u32>(backend_word(frame, 256, 4));
        if (!capture(frame, contract) || !program(frame.pc, begin, end) || !valid_info(info) ||
            (fcsr & fcsr_reserved) || (fcsr & enabled_causes(fcsr))) return false;
        Image image;
        if (!InfoCodec::encode_into(info, image.bytes, info_offset)) return false;
        put(image.bytes, stack_offset, stack.pointer, 8);
        put(image.bytes, stack_offset+8, stack.size, 8);
        put(image.bytes, stack_offset+16, stack.flags, 4);
        for (usize reg = 1; reg < 32; ++reg)
            if (reg != 26 && reg != 27)
                put(image.bytes, context_offset+8*reg, reg == 29 ? frame.sp : frame.registers[reg], 8);
        for (usize i = 0; i < 256; ++i) image.bytes[fp_offset+i] = static_cast<std::byte>(frame.fp_state[i]);
        put(image.bytes, hi_offset, backend_word(frame, 272, 8), 8);
        put(image.bytes, lo_offset, backend_word(frame, 280, 8), 8);
        put(image.bytes, pc_offset, frame.pc, 8);
        put(image.bytes, fcsr_offset, fcsr, 4);
        put(image.bytes, used_math_offset, used_fp_fr1, 4);
        const auto bytes = MaskCodec::encode(MaskCodec::sanitize(mask));
        for (usize i = 0; i < bytes.size(); ++i) image.bytes[mask_offset+i] = bytes[i];
        out = image;
        return true;
    }
    [[nodiscard]] static bool setup_handler(const Frame &current, Capture contract, Placement placement,
                                           Info info, u64 handler, u64 restorer,
                                           u64 begin, u64 end, Frame &out)
    {
        const u32 fcsr = static_cast<u32>(backend_word(current, 256, 4));
        if (!capture(current, contract) || !valid_info(info) || (fcsr & fcsr_reserved) ||
            (fcsr & enabled_causes(fcsr)) || !program(handler, begin, end) ||
            !program(restorer, begin, end) || !range(restorer, 8, begin, end) ||
            (placement.address & 15) || !range(placement.address, frame_bytes, begin, end)) return false;
        Frame candidate = current;
        candidate.pc = candidate.registers[25] = static_cast<uptr>(handler);
        candidate.sp = candidate.registers[29] = static_cast<uptr>(placement.address);
        candidate.registers[0] = 0;
        candidate.registers[4] = static_cast<uptr>(info.signal);
        candidate.registers[5] = static_cast<uptr>(placement.address+info_offset);
        candidate.registers[6] = static_cast<uptr>(placement.address+uc_offset);
        candidate.registers[31] = static_cast<uptr>(restorer);
        out = candidate;
        return true;
    }
    template <typename Read>
    [[nodiscard]] static bool decode(u64 syscall_sp, const Frame &current, Capture contract,
                                    u64 begin, u64 end, Read &&read, ReturnPlan &out)
    {
        if (!capture(current, contract) || (syscall_sp & 15) || !range(syscall_sp, frame_bytes, begin, end)) return false;
        Image image;
        if (!read(syscall_sp, std::span<std::byte>{image.bytes})) return false;
        const auto &bytes = image.bytes;
        const u64 pc = get(bytes, pc_offset, 8), sp = get(bytes, context_offset+29*8, 8);
        if (!program(pc, begin, end) || !stack_pointer(sp, begin, end) ||
            get(bytes, used_math_offset, 4) != used_fp_fr1 || get(bytes, context_offset+592, 4)) return false;
        for (usize i = 1; i < 4; ++i)
            if (get(bytes, hi_offset+8*i, 8) || get(bytes, lo_offset+8*i, 8)) return false;
        const u32 fcsr = static_cast<u32>(get(bytes, fcsr_offset, 4));
        if (fcsr & fcsr_reserved) return false;
        ReturnPlan candidate;
        candidate.frame = current;
        for (usize reg = 1; reg < 32; ++reg)
            if (reg != 26 && reg != 27)
                candidate.frame.registers[reg] = static_cast<uptr>(get(bytes, context_offset+reg*8, 8));
        candidate.frame.registers[0] = 0;
        candidate.frame.pc = static_cast<uptr>(pc);
        candidate.frame.sp = candidate.frame.registers[29] = static_cast<uptr>(sp);
        for (usize i = 0; i < 256; ++i) candidate.frame.fp_state[i] = static_cast<u8>(bytes[fp_offset+i]);
        set_backend_word(candidate.frame, 272, get(bytes, hi_offset, 8), 8);
        set_backend_word(candidate.frame, 280, get(bytes, lo_offset, 8), 8);
        const u32 enabled = enabled_causes(fcsr);
        const bool pending = fcsr & enabled;
        const u32 normalized = pending ? fcsr & ~enabled : fcsr;
        set_backend_word(candidate.frame, 256, normalized, 4);
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
        candidate.stack = {get(bytes, stack_offset, 8), get(bytes, stack_offset+8, 8),
                           static_cast<u32>(get(bytes, stack_offset+16, 4))};
        // CP0 status, TLS, reserved k0/k1 and unused backend bytes remain trusted.
        // Unused uc_flags/link and sc_reserved confer no authority.
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
} // namespace ok::user::native
