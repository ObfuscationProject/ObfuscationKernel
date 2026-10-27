#pragma once
// LoongArch64 native signal frames, delivery and return decoding.
#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"
#include "ok/user/native_signal_delivery.hpp"
#include "ok/user/native_signal_altstack.hpp"
#include "ok/user/native_signal_force.hpp"

namespace ok::user::native
{
// Linux LoongArch64 RT frame for the current scalar-FPU backend. This bounded
// value codec neither installs a syscall nor proves a trampoline mapping.
struct Loongarch64SignalFrame
{
    using Frame = arch::execution::UserFrame;
    using MaskCodec = SignalMaskCodec<abi::linux_user::Profile::loongarch64>;
    using InfoCodec = SignalInfoCodec<abi::linux_user::Profile::loongarch64>;
    static_assert(sizeof(uptr) == 8);
    static constexpr usize frame_bytes = 576, image_bytes = 880;
    static constexpr usize info_offset = 0, ucontext_offset = 128, stack_offset = 144;
    static constexpr usize mask_offset = 168, context_offset = 304, pc_offset = 304;
    static constexpr usize registers_offset = 312, flags_offset = 568;
    static constexpr usize extension_offset = 576, header_bytes = 16;
    static constexpr usize fpu_bytes = 272, fpu_record_bytes = 288;
    static constexpr usize fp_offset = 592, fcc_offset = 848, fcsr_offset = 856, end_offset = 864;
    static constexpr u32 fpu_magic = 0x46505501, used_fp = 1;
    static constexpr u32 address_error_flags = 0xc0000000, fcsr_reserved = 0xe0e0fce0;
    static constexpr u64 user_ceiling = u64{1} << 47;

    struct Stack
    {
        u64 pointer{};
        u64 size{};
        u32 flags{2};
        constexpr bool operator==(const Stack &) const = default;
    };
    struct Info { i32 signal{}; i32 code{}; u32 sender_pid{}; u32 sender_uid{}; i32 child_status{}; };
    struct Placement { u64 address{}; constexpr bool operator==(const Placement &) const = default; };
    struct Image { std::array<std::byte, image_bytes> bytes{}; };
    struct FcsrPatch
    {
        u64 address{};
        u32 value{};
        bool required{};
        constexpr bool operator==(const FcsrPatch &) const = default;
    };
    struct Restored
    {
        Frame frame{};
        SignalMask mask{};
        Stack stack{};
        // Linux fcsr_pending writes the cleared causes to the user frame and
        // requests SIGFPE. Complete this checked write before committing any
        // live restored state; arrange the signal before the next user entry.
        FcsrPatch fcsr_patch{};
        bool pending_fp_exception{};
    };

    static constexpr bool window(u64 begin, u64 end)
    {
        return begin < end && end <= user_ceiling;
    }
    static constexpr bool range(u64 address, u64 size, u64 begin, u64 end)
    {
        return window(begin, end) && address >= begin && address < end && size <= end - address;
    }
    static constexpr bool program_state(u64 pc, u64 begin, u64 end)
    {
        return range(pc, 4, begin, end) && !(pc & 3);
    }
    static constexpr bool user_state(u64 pc, u64 sp, u64 begin, u64 end)
    {
        return program_state(pc, begin, end) && sp > begin && sp <= end && !(sp & 15);
    }
    static constexpr bool valid_info(Info info)
    {
        return InfoCodec::template frame_valid<true>(info);
    }
    static constexpr bool place(u64 selected_top, u64 begin, u64 end, Placement &out)
    {
        if (!window(begin, end) || selected_top <= begin || selected_top > end) return false;
        const u64 top = selected_top & ~u64{15};
        if (top < begin || top - begin < image_bytes) return false;
        const u64 address = top - image_bytes;
        if (!range(address, image_bytes, begin, end)) return false;
        out = {address};
        return true;
    }
    template <usize N>
    static constexpr u64 get(const std::array<std::byte, N> &bytes, usize offset, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i) value |= static_cast<u64>(bytes[offset+i]) << (8*i);
        return value;
    }
    template <usize N>
    static constexpr void put(std::array<std::byte, N> &bytes, usize offset, u64 value, usize count)
    {
        for (usize i = 0; i < count; ++i) bytes[offset+i] = static_cast<std::byte>(value >> (8*i));
    }
    static u32 backend_fcsr(const Frame &frame)
    {
        u32 value{};
        for (usize i = 0; i < 4; ++i) value |= u32{frame.fp_state[256+i]} << (8*i);
        return value;
    }
    static void set_backend_fcsr(Frame &frame, u32 value)
    {
        for (usize i = 0; i < 4; ++i) frame.fp_state[256+i] = static_cast<u8>(value >> (8*i));
    }
    static bool encode(const Frame &frame, SignalMask mask, Stack stack, Info info,
                       u64 begin, u64 end, Image &out)
    {
        // Capture SP verbatim: selection of a valid alternate stack may let
        // the handler repair an invalid interrupted SP. It is not a destination.
        if (!program_state(frame.pc, begin, end) || !valid_info(info) ||
            (backend_fcsr(frame) & fcsr_reserved)) return false;
        for (usize i = 0; i < 8; ++i) if (frame.fp_state[264+i] > 1) return false;
        Image image;
        if (!InfoCodec::encode_into(info, image.bytes, info_offset)) return false;
        put(image.bytes, stack_offset, stack.pointer, 8);
        put(image.bytes, stack_offset+8, stack.flags, 4);
        put(image.bytes, stack_offset+16, stack.size, 8);
        mask = MaskCodec::sanitize(mask);
        put(image.bytes, mask_offset, mask.words[0], 8);
        put(image.bytes, pc_offset, frame.pc, 8);
        for (usize i = 1; i < 32; ++i)
            put(image.bytes, registers_offset+8*i, i == 3 ? frame.sp : frame.registers[i], 8);
        put(image.bytes, flags_offset, used_fp, 4);
        put(image.bytes, extension_offset, fpu_magic, 4);
        put(image.bytes, extension_offset+4, fpu_record_bytes, 4);
        for (usize i = 0; i < 256; ++i) image.bytes[fp_offset+i] = static_cast<std::byte>(frame.fp_state[i]);
        for (usize i = 0; i < 8; ++i) image.bytes[fcc_offset+i] = static_cast<std::byte>(frame.fp_state[264+i]);
        put(image.bytes, fcsr_offset, backend_fcsr(frame), 4);
        // r0, all padding, uc_flags/uc_link and the terminal header stay zero.
        out = image;
        return true;
    }
    template <typename Read>
    static bool decode(u64 syscall_sp, const Frame &current, u64 begin, u64 end,
                       Read &&read, Restored &out)
    {
        if ((syscall_sp & 15) || !range(syscall_sp, image_bytes, begin, end)) return false;
        Image image;
        if (!read(syscall_sp, std::span<std::byte>{image.bytes})) return false;
        const auto &bytes = image.bytes;
        const u64 pc = get(bytes, pc_offset, 8), sp = get(bytes, registers_offset+3*8, 8);
        if (!user_state(pc, sp, begin, end)) return false;
        const u32 flags = static_cast<u32>(get(bytes, flags_offset, 4));
        // Current native execution always retains scalar FPU state. Unknown
        // flag bits and no-FPU/lazy-FPU restoration are outside this subset.
        if (!(flags & used_fp) || (flags & ~(used_fp | address_error_flags))) return false;
        if (get(bytes, extension_offset, 4) != fpu_magic ||
            get(bytes, extension_offset+4, 4) != fpu_record_bytes ||
            get(bytes, end_offset, 4) || get(bytes, end_offset+4, 4)) return false;
        u32 csr = static_cast<u32>(get(bytes, fcsr_offset, 4));
        // TM/LBT and all other unretained/reserved FCSR bits are not admitted.
        // This is an explicit backend subset, not a claim Linux rejects them.
        if (csr & fcsr_reserved) return false;
        Restored candidate;
        candidate.frame = current;
        candidate.frame.registers[0] = 0;
        for (usize i = 1; i < 32; ++i) candidate.frame.registers[i] = get(bytes, registers_offset+8*i, 8);
        candidate.frame.pc = pc;
        candidate.frame.sp = sp;
        candidate.frame.fs_base = candidate.frame.registers[2];
        // User sigcontext cannot change backend privilege flags or gs_base.
        candidate.mask.words[0] = get(bytes, mask_offset, 8);
        candidate.mask = MaskCodec::sanitize(candidate.mask);
        candidate.stack = {get(bytes, stack_offset, 8), get(bytes, stack_offset+16, 8),
                           static_cast<u32>(get(bytes, stack_offset+8, 4))};
        for (usize i = 0; i < 256; ++i) candidate.frame.fp_state[i] = static_cast<u8>(bytes[fp_offset+i]);
        // MOVGR2CF consumes the low bit of each byte; Linux's restore assembly
        // extracts bytes without rejecting unused high bits.
        for (usize i = 0; i < 8; ++i) candidate.frame.fp_state[264+i] = static_cast<u8>(bytes[fcc_offset+i]) & 1;
        const u32 enabled_causes = (csr & 0x1f) << 24;
        if (csr & enabled_causes)
        {
            csr &= ~enabled_causes;
            candidate.pending_fp_exception = true;
            candidate.fcsr_patch = {syscall_sp+fcsr_offset, csr, true};
        }
        set_backend_fcsr(candidate.frame, csr);
        out = candidate;
        return true;
    }
    static bool setup_handler(const Frame &current, Placement placement, Info info, u64 handler,
                              u64 mapped_restorer, bool siginfo, u64 begin, u64 end, Frame &out)
    {
        (void)siginfo;
        if ((placement.address & 15) || !range(placement.address, image_bytes, begin, end) ||
            !user_state(handler, placement.address, begin, end) ||
            !program_state(mapped_restorer, begin, end) || !valid_info(info)) return false;
        Frame candidate = current;
        candidate.registers[0] = 0;
        candidate.registers[1] = mapped_restorer;
        candidate.registers[3] = placement.address;
        candidate.registers[4] = static_cast<u64>(info.signal);
        // Linux supplies all three arguments even when SA_SIGINFO is absent.
        candidate.registers[5] = placement.address+info_offset;
        candidate.registers[6] = placement.address+ucontext_offset;
        candidate.sp = placement.address;
        candidate.pc = handler;
        out = candidate;
        return true;
    }
    template <typename Write>
    static bool prepare(const Frame &current, SignalMask mask, Stack stack, Info info, u64 selected_top,
                        u64 handler, u64 mapped_restorer, bool siginfo, u64 begin, u64 end,
                        Write &&write, Placement &placement_out, Frame &frame_out)
    {
        Placement placement;
        Image image;
        Frame frame;
        if (!place(selected_top, begin, end, placement) ||
            !encode(current, mask, stack, info, begin, end, image) ||
            !setup_handler(current, placement, info, handler, mapped_restorer, siginfo, begin, end, frame) ||
            !write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        // A failed usercopy may have written partial user bytes. It never
        // publishes handler state or consumes a pending signal through this API.
        placement_out = placement;
        frame_out = frame;
        return true;
    }
};

struct Loongarch64SignalFramePolicy
{
    using Frame = Loongarch64SignalFrame;
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
        Frame::Placement placement;
        if (!Frame::place(top, begin, end, placement) ||
            ((nested || entering) && !altstack.physically_contains(placement.address))) return false;
        const Frame::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        Frame::Image image;
        arch::execution::UserFrame next;
        if (!Frame::encode(frame, blocked, {altstack.base, altstack.size, altstack.flags}, sender, begin, end, image) ||
            !Frame::setup_handler(frame, placement, sender, action.handler, mapped_restorer,
                                  (action.flags & 4) != 0, begin, end, next)) return false;
        if (!write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        out = next;
        return true;
    }
};

using Loongarch64SignalDelivery = SignalDelivery<abi::linux_user::Profile::loongarch64, Loongarch64SignalFramePolicy>;

struct Loongarch64SignalReturn
{
    using Frame = arch::execution::UserFrame;
    using Codec = Loongarch64SignalFrame;

    // Caller serializes all task state and user copies. After the last fallible
    // operation, publish the queue, disposition, frame, mask and alternate stack
    // before any next user entry. Callbacks cannot mutate these owners.
    template <typename Queue, typename Read, typename Write, typename CanEnter, typename SizeValid>
    static bool restore(Queue &pending, sched::SignalActionTable &actions,
                        sched::SignalMask &blocked, Frame &frame, sched::SignalAltstack &stack,
                        u64 begin, u64 end, Read &&read, Write &&write,
                        CanEnter &&can_enter, SizeValid &&size_valid)
    {
        Codec::Restored staged;
        if (!Codec::decode(frame.sp, frame, begin, end, read, staged) || !can_enter(staged.frame)) return false;
        auto next_stack = stack;
        apply_restored_altstack<abi::linux_user::Profile::loongarch64>(next_stack,
            {staged.stack.pointer, staged.stack.size, staged.stack.flags}, staged.frame.sp, size_valid);
        if (staged.fcsr_patch.required)
        {
            std::array<std::byte, 4> bytes{};
            Codec::put(bytes, 0, staged.fcsr_patch.value, bytes.size());
            if (!write(staged.fcsr_patch.address, std::span<const std::byte>{bytes})) return false;
        }
        if (staged.pending_fp_exception && !force_kernel_signal(pending, actions, staged.mask, 8)) return false;
        frame = staged.frame;
        blocked = staged.mask;
        stack = next_stack;
        return true;
    }
};
} // namespace ok::user::native
