#pragma once
// RISC-V (RV32 and RV64) native signal frames, delivery and return decoding.
#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"
#include "ok/user/native_signal_delivery.hpp"

namespace ok::user::native
{
// Linux MMU RT frames for the retained little-endian RV32GC/RV64GC backends.
// Fixed D state only: no F-only, Q, V/RVV or other extension-state contract.
template <abi::linux_user::Profile Selected>
    requires (Selected == abi::linux_user::Profile::rv32 || Selected == abi::linux_user::Profile::rv64)
struct RiscvSignalFrame
{
    using Frame = arch::execution::UserFrame;
    using MaskCodec = SignalMaskCodec<Selected>;
    using InfoCodec = SignalInfoCodec<Selected>;
    static constexpr usize word_bytes = Selected == abi::linux_user::Profile::rv32 ? 4 : 8;
    static_assert(sizeof(uptr) >= word_bytes);
    static constexpr u64 word_mask = word_bytes == 4 ? 0xffffffffULL : ~u64{};
    static constexpr usize frame_bytes = word_bytes == 4 ? 944 : 1088;
    static constexpr usize uc_offset = 128, stack_offset = uc_offset + 2 * word_bytes;
    static constexpr usize stack_size_offset = stack_offset + 2 * word_bytes;
    static constexpr usize mask_offset = word_bytes == 4 ? 148 : 168;
    static constexpr usize context_offset = word_bytes == 4 ? 288 : 304;
    static constexpr usize fp_offset = context_offset + 32 * word_bytes;
    static constexpr usize fcsr_offset = fp_offset + 256, reserved_offset = fp_offset + 516;
    static constexpr usize magic_offset = fp_offset + 520, record_size_offset = fp_offset + 524;
    static constexpr usize info_pid_offset = word_bytes == 4 ? 12 : 16;
    static constexpr usize restorer_bytes = 8; // Kernel-owned li a7,139; ecall trampoline.

    // Raw stored altstack fields, not query(sp). Selection, AUTODISARM commit,
    // and Linux restore_altstack error suppression belong to the caller.
    struct Stack { u64 base{}; u64 size{}; u32 flags{2}; };
    // Typed sender/child/timer payload; fault and queued-value variants remain unavailable.
    struct Info { i32 signal{}; i32 code{}; u32 sender_pid{}; u32 sender_uid{}; i32 child_status{}; i32 timer{}; i32 overrun{}; u64 value{}; };
    struct Placement { u64 address{}; };
    struct Image { std::array<std::byte, frame_bytes> bytes{}; };
    struct Restored { Frame frame{}; SignalMask mask{}; Stack stack{}; };

    template <usize N>
    static constexpr u64 get(const std::array<std::byte, N> &bytes, usize at, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i) value |= static_cast<u64>(bytes[at + i]) << (8 * i);
        return value;
    }
    template <usize N>
    static constexpr void put(std::array<std::byte, N> &bytes, usize at, u64 value, usize count)
    {
        for (usize i = 0; i < count; ++i) bytes[at + i] = static_cast<std::byte>(value >> (8 * i));
    }
    static constexpr bool range(u64 address, u64 size, u64 begin, u64 end)
    {
        return begin < end && end <= word_mask && begin <= address && address < end && size <= end - address;
    }
    static constexpr bool valid_pc(u64 pc, u64 begin, u64 end)
    {
        return !(pc & 1) && range(pc, 2, begin, end);
    }
    static constexpr bool valid_sp(u64 sp, u64 begin, u64 end)
    {
        return begin < end && end <= word_mask && begin < sp && sp <= end;
    }
    static constexpr bool valid_info(Info info)
    {
        return InfoCodec::template frame_valid<>(info);
    }
    static constexpr bool place(u64 top, u64 begin, u64 end, Placement &out)
    {
        if (!valid_sp(top, begin, end) || top - begin < frame_bytes) return false;
        const u64 address = (top - frame_bytes) & ~u64{15};
        if (!range(address, frame_bytes, begin, end)) return false;
        out = {address};
        return true;
    }

    // Only copy the resulting image after this preparation succeeds. Byte
    // initialization also scrubs siginfo/stack/ucontext/D padding and unused Q space.
    static bool encode(const Frame &frame, SignalMask mask, Stack stack, Info info,
                       u64 begin, u64 end, Image &out)
    {
        // Save captured SP verbatim; an alternate-stack handler may repair it.
        // RV32 must still fit the ABI word. Decode validates the restored SP.
        if (!valid_pc(frame.pc, begin, end) || frame.sp > word_mask ||
            !valid_info(info) || stack.base > word_mask || stack.size > word_mask) return false;
        for (usize i = 1; i < 32; ++i)
            if (i != 2 && frame.registers[i] > word_mask) return false;
        Image candidate;
        if (!InfoCodec::encode_into(info, candidate.bytes, 0)) return false;
        put(candidate.bytes, stack_offset, stack.base, word_bytes);
        put(candidate.bytes, stack_offset + word_bytes, stack.flags, 4);
        put(candidate.bytes, stack_size_offset, stack.size, word_bytes);
        const auto encoded_mask = MaskCodec::encode(mask);
        for (usize i = 0; i < encoded_mask.size(); ++i) candidate.bytes[mask_offset + i] = encoded_mask[i];
        put(candidate.bytes, context_offset, frame.pc, word_bytes); // Slot zero is PC, not x0.
        for (usize i = 1; i < 32; ++i)
            put(candidate.bytes, context_offset + i * word_bytes, i == 2 ? frame.sp : frame.registers[i], word_bytes);
        for (usize i = 0; i < 256; ++i) candidate.bytes[fp_offset + i] = static_cast<std::byte>(frame.fp_state[i]);
        u32 fcsr{};
        for (usize i = 0; i < 4; ++i) fcsr |= static_cast<u32>(frame.fp_state[256 + i]) << (8 * i);
        put(candidate.bytes, fcsr_offset, fcsr & 0xffU, 4); // FCSR reserved bits are hardware-ignored.
        out = candidate;
        return true;
    }

    // Linux ignores sa_restorer on these ABIs. The caller must supply an
    // executable, kernel-owned user trampoline with the rt_sigreturn sequence.
    // Numeric range checks do not prove its mapping or instruction bytes.
    static bool stage_handler(const Frame &interrupted, Placement placement, u64 handler,
                              u64 trampoline, Info info, u64 begin, u64 end, Frame &out)
    {
        if ((placement.address & 15) || !range(placement.address, frame_bytes, begin, end) ||
            !valid_pc(handler, begin, end) || !valid_pc(trampoline, begin, end) ||
            !range(trampoline, restorer_bytes, begin, end) || !valid_info(info)) return false;
        Frame candidate = interrupted;
        candidate.registers[0] = 0;
        candidate.pc = static_cast<uptr>(handler);
        candidate.sp = static_cast<uptr>(placement.address);
        candidate.registers[1] = static_cast<uptr>(trampoline);
        candidate.registers[2] = candidate.sp;
        candidate.registers[10] = static_cast<uptr>(info.signal);
        candidate.registers[11] = candidate.sp;
        candidate.registers[12] = static_cast<uptr>(placement.address + uc_offset);
        // TP (x4) and every other unmodified register retain interrupted values.
        out = candidate;
        return true;
    }

    // User-copy callbacks bind statically; decode stages everything before
    // publication. A false result must become a bad-frame user fault, never a
    // successful rt_sigreturn or an ordinary EFAULT return into its trampoline.
    template <typename Read>
    static bool decode(u64 syscall_sp, const Frame &current, u64 begin, u64 end,
                       Read &&read, Restored &out)
    {
        if (!range(syscall_sp, frame_bytes, begin, end)) return false;
        Image image;
        if (!read(syscall_sp, std::span<std::byte>{image.bytes})) return false;
        const auto &bytes = image.bytes;
        if (get(bytes, reserved_offset, 4) || get(bytes, magic_offset, 4) || get(bytes, record_size_offset, 4)) return false;
        const u64 pc = get(bytes, context_offset, word_bytes), sp = get(bytes, context_offset + 2 * word_bytes, word_bytes);
        if (!valid_pc(pc, begin, end) || !valid_sp(sp, begin, end)) return false;
        Restored candidate;
        candidate.frame = current; // No mstatus field exists in the Linux frame.
        candidate.frame.registers[0] = 0;
        for (usize i = 1; i < 32; ++i)
            candidate.frame.registers[i] = static_cast<uptr>(get(bytes, context_offset + i * word_bytes, word_bytes));
        candidate.frame.pc = static_cast<uptr>(pc);
        candidate.frame.sp = static_cast<uptr>(sp);
        // TP is user-editable through Linux mcontext; caller must sync TCB TLS.
        candidate.frame.fp_state.fill(0);
        for (usize i = 0; i < 256; ++i) candidate.frame.fp_state[i] = static_cast<u8>(bytes[fp_offset + i]);
        candidate.frame.fp_state[256] = static_cast<u8>(get(bytes, fcsr_offset, 4) & 0xffU);
        std::array<std::byte, MaskCodec::bytes> mask{};
        for (usize i = 0; i < mask.size(); ++i) mask[i] = bytes[mask_offset + i];
        candidate.mask = MaskCodec::sanitize(MaskCodec::decode(mask));
        candidate.stack = {get(bytes, stack_offset, word_bytes), get(bytes, stack_size_offset, word_bytes),
                           static_cast<u32>(get(bytes, stack_offset + word_bytes, 4))};
        // Linux ignores uc_flags/uc_link, public-mask expansion and D/unused-Q
        // padding; only the explicit reserved word and END header are mandatory.
        out = candidate;
        return true;
    }
};

using Rv32SignalFrame = RiscvSignalFrame<abi::linux_user::Profile::rv32>;
using Rv64SignalFrame = RiscvSignalFrame<abi::linux_user::Profile::rv64>;

template <abi::linux_user::Profile Profile>
    requires (Profile == abi::linux_user::Profile::rv32 || Profile == abi::linux_user::Profile::rv64)
struct RiscvSignalFramePolicy
{
    using Frame = RiscvSignalFrame<Profile>;
    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &) { return true; }

    template <typename Write>
    static bool prepare(const arch::execution::UserFrame &frame, sched::SignalMask blocked,
                        sched::PendingSignalInfo info, const sched::SignalAction &action,
                        const sched::SignalAltstack &altstack, u64 begin, u64 end, u32,
                        Write &&write, arch::execution::UserFrame &out, u64 mapped_restorer)
    {
        const bool nested = altstack.on_stack(frame.sp);
        const bool entering = (action.flags & 0x08000000) && altstack.status(frame.sp) == 0;
        u64 top = frame.sp;
        if (entering)
        {
            if (altstack.base >= end || altstack.size > end - altstack.base) return false;
            top = altstack.base + altstack.size;
        }
        typename Frame::Placement placement;
        if (!Frame::place(top, begin, end, placement) ||
            ((nested || entering) && !altstack.physically_contains(placement.address))) return false;
        const typename Frame::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status,
            info.timer, info.overrun, info.value};
        typename Frame::Image image;
        arch::execution::UserFrame next;
        if (!Frame::encode(frame, blocked, {altstack.base, altstack.size, altstack.flags}, sender, begin, end, image) ||
            !Frame::stage_handler(frame, placement, action.handler, mapped_restorer, sender, begin, end, next)) return false;
        if (!write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        out = next;
        return true;
    }
};

template <abi::linux_user::Profile Profile>
using RiscvSignalDelivery = SignalDelivery<Profile, RiscvSignalFramePolicy<Profile>>;
} // namespace ok::user::native
