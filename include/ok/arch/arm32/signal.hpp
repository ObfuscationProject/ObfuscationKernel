#pragma once
// ARM32 native signal frames, delivery and return decoding.
#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"
#include "ok/arch/arm32/fp_state.hpp"
#include "ok/user/native_signal_delivery.hpp"
#include "ok/user/native_signal_altstack.hpp"

namespace ok::user::native
{
enum class Arm32SignalFormat : u8 { legacy, realtime };

// Linux little-endian ARMv7 EABI, with ARM and Thumb interworking and D32 VFP.
// A value codec only: mapping permissions, cache maintenance, signal policy,
// usercopy and atomic publication remain the caller's responsibilities.
template <Arm32SignalFormat Format>
struct Arm32SignalFrame
{
    static_assert(Format == Arm32SignalFormat::legacy || Format == Arm32SignalFormat::realtime);
    using Frame = arch::execution::UserFrame;
    using MaskCodec = SignalMaskCodec<abi::linux_user::Profile::arm32>;
    using InfoCodec = SignalInfoCodec<abi::linux_user::Profile::arm32>;
    static constexpr bool realtime = Format == Arm32SignalFormat::realtime;
    static constexpr u32 return_syscall = realtime ? 173 : 119;
    static constexpr usize uc_offset = realtime ? 128 : 0;
    static constexpr usize frame_bytes = uc_offset + 760;
    static constexpr usize stack_offset = uc_offset + 8, context_offset = uc_offset + 20;
    static constexpr usize registers_offset = context_offset + 12, mask_offset = uc_offset + 104;
    static constexpr usize extension_offset = uc_offset + 232, retcode_offset = uc_offset + 744;
    static constexpr usize pc_offset = context_offset + 72, sp_offset = context_offset + 64;
    static constexpr usize cpsr_offset = context_offset + 76, vfp_bytes = 288;
    static constexpr usize fp_offset = extension_offset + 8, fpscr_offset = extension_offset + 264;
    static constexpr usize fpexc_offset = extension_offset + 272, end_offset = extension_offset + 288;
    static constexpr u32 vfp_magic = 0x56465001, legacy_uc_flags = 0x5ac3c35a;
    static constexpr u32 thumb_bit = 0x20, it_mask = 0x0600fc00, user_mode = 0x10;
    static constexpr u32 allowed_cpsr = 0xfe0ffc30, ignored_cpsr = 0x140;
    static constexpr u32 fp_enable = 0x40000000, fp_discard = 0x90000000;
    static constexpr u32 fpscr_function_clear = 0x00370000;
    static constexpr u64 word_max = 0xffffffffULL;

    struct Stack
    {
        u64 pointer{}, size{};
        u32 flags{2};
        constexpr bool operator==(const Stack &) const = default;
    };
    struct Info { i32 signal{}, code{}; u32 sender_pid{}, sender_uid{}; i32 child_status{}, timer{}, overrun{}; u64 value{}; };
    // The present backend captures D0-D31 and FPSCR, not user FPEXC/FPINST.
    // The caller must attest this fixed state from trusted backend policy;
    // constructing it is not evidence that the hardware has that state.
    struct ExceptionState
    {
        u32 fpexc{}, fpinst{}, fpinst2{};
        constexpr bool operator==(const ExceptionState &) const = default;
    };
    struct ReturnCode
    {
        u64 entry{}; // Interworking bit zero selects the return ISA.
        bool user_supplied{}; // SA_RESTORER; otherwise a checked kernel trampoline.
    };
    struct Placement { u64 address{}; constexpr bool operator==(const Placement &) const = default; };
    struct Image { std::array<std::byte, frame_bytes> bytes{}; };
    struct Restored
    {
        Frame frame{};
        SignalMask mask{};
        Stack stack{};
        ExceptionState exception_state{};
        // Legacy sigreturn never restores altstack, including its uc_stack edits.
        bool restore_altstack{realtime};
    };

    static constexpr bool range(u64 address, u64 size, u64 begin, u64 end)
    {
        return begin < end && end <= word_max && address >= begin && address < end && size <= end - address;
    }
    static constexpr bool stack_pointer(u64 sp, u64 begin, u64 end)
    {
        // Interrupted/restored SP need not obey a public function's alignment.
        // The sigreturn syscall's frame address itself must remain 8-byte aligned.
        return begin < end && end <= word_max && sp > begin && sp <= end;
    }
    static constexpr bool cpsr(u32 raw, u32 &normalized)
    {
        const u32 value = raw & ~ignored_cpsr; // Linux always clears FIQ/abort masks.
        if ((value & 31) != user_mode || (value & ~allowed_cpsr) ||
            (!(value & thumb_bit) && (value & it_mask))) return false;
        normalized = value;
        return true;
    }
    static constexpr bool program(u64 pc, u64 flags, u64 begin, u64 end)
    {
        u32 normalized{};
        if (flags > word_max || !cpsr(static_cast<u32>(flags), normalized)) return false;
        const u64 instruction = (normalized & thumb_bit) ? 2 : 4;
        return !(pc & (instruction - 1)) && range(pc, instruction, begin, end);
    }
    static constexpr bool entry(u64 tagged, u64 begin, u64 end)
    {
        return program(tagged & ~u64{1}, user_mode | ((tagged & 1) ? thumb_bit : 0), begin, end);
    }
    static constexpr bool valid_info(Info info)
    {
        return InfoCodec::template frame_valid<>(info);
    }
    static constexpr bool fixed_exception(ExceptionState state)
    {
        return state == ExceptionState{fp_enable, 0, 0};
    }
    static constexpr bool place(u64 selected_top, u64 begin, u64 end, Placement &out)
    {
        if (!stack_pointer(selected_top, begin, end) || selected_top - begin < frame_bytes) return false;
        const u64 address = (selected_top - frame_bytes) & ~u64{7};
        if (!range(address, frame_bytes, begin, end)) return false;
        out = {address};
        return true;
    }
    template <usize N>
    static constexpr u64 get(const std::array<std::byte, N> &bytes, usize at, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i) value |= static_cast<u64>(bytes[at+i]) << (8*i);
        return value;
    }
    template <usize N>
    static constexpr void put(std::array<std::byte, N> &bytes, usize at, u64 value, usize count)
    {
        for (usize i = 0; i < count; ++i) bytes[at+i] = static_cast<std::byte>(value >> (8*i));
    }
    static u32 backend_fpscr(const Frame &frame)
    {
        u32 value{};
        for (usize i = 0; i < 4; ++i) value |= u32{frame.fp_state[256+i]} << (8*i);
        return value;
    }
    static void set_backend_fpscr(Frame &frame, u32 value)
    {
        for (usize i = 0; i < 4; ++i) frame.fp_state[256+i] = static_cast<u8>(value >> (8*i));
    }
    static constexpr std::array<u32, 2> restorer_words(bool thumb)
    {
        // Exact two-word slices copied by Linux setup_return. The second Thumb
        // word belongs to the following snippet and is unreachable after return.
        if (thumb) return {realtime ? 0xdf0027adU : 0xdf002777U,
                           realtime ? 0xe59d32f4U : 0xe3a070adU};
        return {realtime ? 0xe3a070adU : 0xe3a07077U,
                realtime ? 0xef9000adU : 0xef900077U};
    }
    static bool encode(const Frame &frame, SignalMask mask, Stack stack, Info info,
                       ExceptionState exception, bool thumb_handler, bool supplied_restorer,
                       u64 begin, u64 end, Image &out)
    {
        if (!program(frame.pc, frame.flags, begin, end) || frame.sp > word_max ||
            !valid_info(info) || stack.pointer > word_max || stack.size > word_max ||
            !fixed_exception(exception)) return false;
        for (usize i = 0; i < 15; ++i)
            if (i != 13 && frame.registers[i] > word_max) return false;
        Image image;
        if constexpr (realtime)
        {
            if (!InfoCodec::encode_into(info, image.bytes, 0)) return false;
            // Raw stored metadata, not sigaltstack query(interrupted SP).
            put(image.bytes, stack_offset, stack.pointer, 4);
            put(image.bytes, stack_offset+4, stack.flags, 4);
            put(image.bytes, stack_offset+8, stack.size, 4);
        }
        else put(image.bytes, uc_offset, legacy_uc_flags, 4);
        mask = MaskCodec::sanitize(mask);
        put(image.bytes, context_offset+8, mask.words[0] & word_max, 4);
        put(image.bytes, mask_offset, mask.words[0], 8);
        for (usize i = 0; i < 16; ++i)
            put(image.bytes, registers_offset+4*i, i == 13 ? frame.sp : i == 15 ? frame.pc : frame.registers[i], 4);
        put(image.bytes, cpsr_offset, frame.flags, 4);
        put(image.bytes, extension_offset, vfp_magic, 4);
        put(image.bytes, extension_offset+4, vfp_bytes, 4);
        for (usize i = 0; i < 256; ++i) image.bytes[fp_offset+i] = static_cast<std::byte>(frame.fp_state[i]);
        put(image.bytes, fpscr_offset, backend_fpscr(frame), 4);
        put(image.bytes, fpexc_offset, exception.fpexc, 4);
        if (!supplied_restorer)
        {
            const auto words = restorer_words(thumb_handler);
            for (usize i = 0; i < 2; ++i) put(image.bytes, retcode_offset+4*i, words[i], 4);
        }
        // Trap metadata, uc_link, legacy uc_stack, unused mask bytes, VFP padding,
        // terminal magic, unsupported extension space and unused retcode stay zero.
        out = image;
        return true;
    }
    static bool setup_handler(const Frame &current, Placement placement, Info info, u64 handler,
                              ReturnCode restorer, u64 begin, u64 end, Frame &out)
    {
        u32 normalized{};
        const bool thumb = handler & 1;
        if (current.flags > word_max || !cpsr(static_cast<u32>(current.flags), normalized) ||
            (placement.address & 7) || !range(placement.address, frame_bytes, begin, end) ||
            !entry(handler, begin, end) || !entry(restorer.entry, begin, end) || !valid_info(info)) return false;
        // Kernel-owned trampolines match Linux's selected handler ISA and need
        // eight checked code bytes. A supplied interworking restorer can differ.
        if (!restorer.user_supplied && (bool(restorer.entry & 1) != thumb ||
            !range(restorer.entry & ~u64{1}, 8, begin, end))) return false;
        Frame candidate = current;
        candidate.pc = static_cast<uptr>(handler & ~u64{1});
        candidate.sp = static_cast<uptr>(placement.address);
        candidate.flags = (normalized & ~(0xff000000U | it_mask | thumb_bit)) | (thumb ? thumb_bit : 0);
        candidate.registers[0] = static_cast<uptr>(info.signal);
        candidate.registers[13] = candidate.sp;
        candidate.registers[14] = static_cast<uptr>(restorer.entry);
        candidate.registers[15] = candidate.pc;
        if constexpr (realtime)
        {
            candidate.registers[1] = candidate.sp;
            candidate.registers[2] = candidate.sp + uc_offset;
        }
        set_backend_fpscr(candidate, backend_fpscr(current) & ~fpscr_function_clear);
        out = candidate;
        return true;
    }
    template <typename Read>
    static bool decode(u64 syscall_sp, const Frame &current, u64 begin, u64 end,
                       Read &&read, Restored &out)
    {
        // Both ARM return syscalls use SP directly. There is no popped return
        // address, no hidden offset, and no dependence on the caller's ISA.
        if ((syscall_sp & 7) || !range(syscall_sp, frame_bytes, begin, end)) return false;
        Image image;
        if (!read(syscall_sp, std::span<std::byte>{image.bytes})) return false;
        const auto &bytes = image.bytes;
        const u64 pc = get(bytes, pc_offset, 4), sp = get(bytes, sp_offset, 4);
        const u32 flags = static_cast<u32>(get(bytes, cpsr_offset, 4));
        u32 normalized{};
        if (!program(pc, flags, begin, end) || !stack_pointer(sp, begin, end) || !cpsr(flags, normalized)) return false;
        if (get(bytes, extension_offset, 4) != vfp_magic ||
            get(bytes, extension_offset+4, 4) != vfp_bytes || get(bytes, end_offset, 4)) return false;
        ExceptionState exception{static_cast<u32>(get(bytes, fpexc_offset, 4)),
                                 static_cast<u32>(get(bytes, fpexc_offset+4, 4)),
                                 static_cast<u32>(get(bytes, fpexc_offset+8, 4))};
        exception.fpexc = (exception.fpexc | fp_enable) & ~fp_discard;
        // Arbitrary exception/trap state, FPINST and iWMMXt are not retained by
        // this backend. Reject them explicitly, without inventing storage slots.
        if (!fixed_exception(exception)) return false;
        Restored candidate;
        candidate.frame = current; // TLS and unused backend state remain trusted.
        for (usize i = 0; i < 16; ++i)
            candidate.frame.registers[i] = static_cast<uptr>(get(bytes, registers_offset+4*i, 4));
        candidate.frame.pc = static_cast<uptr>(pc);
        candidate.frame.sp = static_cast<uptr>(sp);
        candidate.frame.flags = normalized;
        candidate.mask.words[0] = get(bytes, mask_offset, 8);
        candidate.mask = MaskCodec::sanitize(candidate.mask);
        candidate.stack = {get(bytes, stack_offset, 4), get(bytes, stack_offset+8, 4),
                           static_cast<u32>(get(bytes, stack_offset+4, 4))};
        candidate.exception_state = exception;
        for (usize i = 0; i < 256; ++i) candidate.frame.fp_state[i] = static_cast<u8>(bytes[fp_offset+i]);
        set_backend_fpscr(candidate.frame, static_cast<u32>(get(bytes, fpscr_offset, 4)));
        out = candidate;
        return true;
    }
    template <typename Write>
    static bool prepare(const Frame &current, SignalMask mask, Stack stack, Info info,
                        ExceptionState exception, u64 selected_top, u64 handler, ReturnCode restorer,
                        u64 begin, u64 end, Write &&write, Placement &placement_out, Frame &frame_out)
    {
        Placement placement;
        Image image;
        Frame handler_frame;
        if (!place(selected_top, begin, end, placement) ||
            !encode(current, mask, stack, info, exception, handler & 1, restorer.user_supplied, begin, end, image) ||
            !setup_handler(current, placement, info, handler, restorer, begin, end, handler_frame) ||
            !write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        // Partial user writes are possible; no live frame or placement is
        // published until the checked write succeeds completely.
        placement_out = placement;
        frame_out = handler_frame;
        return true;
    }
};

using Arm32LegacySignalFrame = Arm32SignalFrame<Arm32SignalFormat::legacy>;
using Arm32RtSignalFrame = Arm32SignalFrame<Arm32SignalFormat::realtime>;

struct Arm32SignalFramePolicy
{
    using Frame = Arm32RtSignalFrame;
    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &action)
    {
        return action.handler <= 1 || (action.flags & 0x04000000);
    }

    template <Arm32SignalFormat Format, typename Write>
    static bool prepare_kind(const arch::execution::UserFrame &frame, sched::SignalMask blocked,
                             sched::PendingSignalInfo info, const sched::SignalAction &action,
                             const sched::SignalAltstack &altstack, u64 begin, u64 end,
                             Write &&write, arch::execution::UserFrame &out)
    {
        using Codec = Arm32SignalFrame<Format>;
        using Fp = arch::execution::Arm32FpState;
        if (!(action.flags & 0x04000000) || !Fp::admitted(frame)) return false;
        const bool nested = altstack.on_stack(frame.sp);
        const bool entering = (action.flags & 0x08000000) && altstack.status(frame.sp) == 0;
        u64 top = frame.sp;
        if (entering)
        {
            if (altstack.base >= end || altstack.size > end - altstack.base) return false;
            top = altstack.base + altstack.size;
        }
        typename Codec::Placement placement;
        if (!Codec::place(top, begin, end, placement) ||
            ((nested || entering) && !altstack.physically_contains(placement.address))) return false;
        // FPEXC comes from the actual trap capture. This synchronous D32 profile
        // excludes pending exception/FPINST state; it does not claim to capture it.
        const typename Codec::ExceptionState exception{Fp::fpexc(frame), 0, 0};
        const typename Codec::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status,
            info.timer, info.overrun, info.value};
        typename Codec::Image image;
        arch::execution::UserFrame next;
        if (!Codec::encode(frame, blocked, {altstack.base, altstack.size, altstack.flags}, sender,
                           exception, action.handler & 1, true, begin, end, image) ||
            !Codec::setup_handler(frame, placement, sender, action.handler, {action.restorer, true},
                                  begin, end, next) ||
            !write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        out = next;
        return true;
    }

    template <typename Write>
    static bool prepare(const arch::execution::UserFrame &frame, sched::SignalMask blocked,
                        sched::PendingSignalInfo info, const sched::SignalAction &action,
                        const sched::SignalAltstack &altstack, u64 begin, u64 end, u32,
                        Write &&write, arch::execution::UserFrame &out, u64)
    {
        if (action.flags & 4)
            return prepare_kind<Arm32SignalFormat::realtime>(frame, blocked, info, action, altstack,
                                                             begin, end, write, out);
        return prepare_kind<Arm32SignalFormat::legacy>(frame, blocked, info, action, altstack,
                                                      begin, end, write, out);
    }
};

using Arm32SignalDelivery = SignalDelivery<abi::linux_user::Profile::arm32, Arm32SignalFramePolicy>;

struct Arm32SignalReturn
{
    using Frame = arch::execution::UserFrame;
    using Operation = abi::linux_user::Operation;
    struct Restored
    {
        Frame frame{};
        sched::SignalMask mask{};
        sched::SignalAltstack stack{};
        bool restore_stack{};
    };

    template <Arm32SignalFormat Format, typename Read, typename CanEnter>
    static bool decode_kind(const Frame &current, u64 begin, u64 end, Read &&read,
                            CanEnter &&can_enter, Restored &out)
    {
        using Codec = Arm32SignalFrame<Format>;
        using Fp = arch::execution::Arm32FpState;
        typename Codec::Restored decoded;
        if (!Codec::decode(current.sp, current, begin, end, read, decoded)) return false;
        // The byte codec predates this architecture-private retained slot.
        // Publish the validated, Linux-normalized returned state explicitly.
        Fp::set_fpexc(decoded.frame, decoded.exception_state.fpexc);
        if (!Fp::admitted(decoded.frame) || !can_enter(decoded.frame)) return false;
        Restored staged{decoded.frame, decoded.mask, {}, decoded.restore_altstack};
        if (decoded.restore_altstack)
            staged.stack = {decoded.stack.pointer, decoded.stack.size, decoded.stack.flags};
        out = staged;
        return true;
    }

    // Both syscalls use the current SP directly. The callback must admit the
    // staged executable PC and writable SP-1 before any live state is changed.
    // Apply raw RT altstack metadata only after success; legacy preserves it.
    template <typename Read, typename CanEnter>
    static bool decode(Operation operation, const Frame &current, u64 begin, u64 end,
                       Read &&read, CanEnter &&can_enter, Restored &out)
    {
        if (operation == Operation::sigreturn)
            return decode_kind<Arm32SignalFormat::legacy>(current, begin, end, read, can_enter, out);
        if (operation == Operation::rt_sigreturn)
            return decode_kind<Arm32SignalFormat::realtime>(current, begin, end, read, can_enter, out);
        return false;
    }
};
} // namespace ok::user::native
