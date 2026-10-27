#pragma once
// PPC32 native signal frames, delivery and return decoding, with the private readiness state.
#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"
#include "ok/user/native_signal_delivery.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#include "ok/user/native_signal_altstack.hpp"
#include "ok/arch/ppc32_private_signal.hpp"
#include "ok/abi/linux_native.hpp"
#include "ok/user/native_ppc32_elf_admission.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"

namespace ok::user::native
{
enum class Ppc32SignalFormat : u8 { legacy, realtime };

// Native big-endian PPC32, eager e500v2 SPE, no classic FPU/VMX/VSX/TM.
// This codec is a staged value operation, not a capability or mapping probe.
template <Ppc32SignalFormat Format>
struct Ppc32SignalFrame
{
    static_assert(Format == Ppc32SignalFormat::legacy || Format == Ppc32SignalFormat::realtime);
    using Frame = arch::execution::UserFrame;
    using MaskCodec = SignalMaskCodec<abi::linux_user::Profile::ppc>;
    using InfoCodec = SignalInfoCodec<abi::linux_user::Profile::ppc>;
    static constexpr bool realtime = Format == Ppc32SignalFormat::realtime;
    static constexpr usize context_bytes = 992, abi_gap_bytes = 224;
    static constexpr usize frame_bytes = realtime ? 1536 : 1248;
    static constexpr usize handler_gap = realtime ? 80 : 64;
    static constexpr usize allocation_bytes = handler_gap + frame_bytes;
    // The upper ABI gap preserves interrupted below-SP saves. Never write it.
    static constexpr usize image_bytes = allocation_bytes - abi_gap_bytes;
    static constexpr usize prefix_bytes = realtime ? 320 : 32;
    static constexpr usize context_offset = handler_gap + prefix_bytes;
    static constexpr usize uc_offset = 128, uc_stack_offset = 8;
    static constexpr usize uc_regs_offset = 48, uc_mask_offset = 52;
    static constexpr usize pc_slot = 32, msr_slot = 33, orig_r3_slot = 34;
    static constexpr usize ctr_slot = 35, link_slot = 36, xer_slot = 37, cr_slot = 38;
    static constexpr usize mq_slot = 39, trap_slot = 40, dar_slot = 41, dsisr_slot = 42, result_slot = 43;
    static constexpr usize fp_offset = 192, fp_bytes = 264, pad_offset = 456;
    static constexpr usize spe_offset = 464, spe_acc_offset = spe_offset + 128;
    static constexpr usize spefscr_offset = spe_offset + 136;
    static constexpr u32 return_syscall = realtime ? 172 : 119;
    static constexpr u32 msr_spe = 0x02000000, msr_pr = 0x4000;
    static constexpr u32 unsupported_msr = 0x80802901; // CM, VSX, FP, FE0/1, LE.
    static constexpr u32 spefscr_reserved = 0x00c00080;
    static constexpr u64 word_max = 0xffffffffULL, user_ceiling = 0x80000000ULL;

    enum class Hardware : u8 { unverified, native32_be_e500v2_eager_spe_only };
    enum class Resume : u8 { unverified, normalized_no_restart, prepared_syscall_restart };
    struct Contract
    {
        Hardware hardware{Hardware::unverified};
        Resume resume{Resume::unverified};
    };
    // Current UserFrame does not contain these Linux pt_regs diagnostics.
    // A future adapter must capture them explicitly, not alias unrelated slots.
    struct LinuxTrapMetadata
    {
        u32 original_r3{}, trap{}, dar{}, dsisr{}, result{};
        constexpr bool operator==(const LinuxTrapMetadata &) const = default;
    };
    struct Capture
    {
        Contract contract{};
        LinuxTrapMetadata metadata{};
        bool metadata_supplied{};
    };
    struct Stack
    {
        u64 pointer{}, size{};
        u32 flags{2};
        constexpr bool operator==(const Stack &) const = default;
    };
    struct Info { i32 signal{}, code{}; u32 sender_pid{}, sender_uid{}; i32 child_status{}; };
    struct Placement
    {
        u64 frame_address{}, stack_pointer{};
        constexpr bool operator==(const Placement &) const = default;
    };
    struct Image { std::array<std::byte, image_bytes> bytes{}; };
    struct [[nodiscard]] ReturnPlan
    {
        Frame frame{};
        SignalMask mask{};
        Stack stack{};
        // Untrusted ABI diagnostics only; never drive privileged trap dispatch.
        LinuxTrapMetadata metadata{};
        bool restore_altstack{realtime};
        // Linux TIF_RESTOREALL + trap_norestart semantics. Do not overwrite r3
        // or CR0.SO with ordinary syscall completion after publishing this plan.
        static constexpr bool restore_all_no_restart = true;
        // Native CONFIG_SPE restore copies SPEFSCR verbatim, including enabled
        // sticky conditions. It has no fcsr_pending user patch or force_sig.
        static constexpr bool checked_user_patch_required = false;
        static constexpr bool forced_fp_signal_required = false;
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
        // Linux restores interrupted r1 verbatim. Its alignment is independent
        // of the aligned signal frame we create (EABI callees may use 8 bytes).
        // Runtime admission separately requires writable memory below r1.
        return window(begin, end) && sp > begin && sp <= end;
    }
    static constexpr bool valid_info(Info info)
    {
        return InfoCodec::template frame_valid<>(info);
    }
    template <usize N>
    static constexpr u64 get(const std::array<std::byte, N> &bytes, usize at, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i) value = (value << 8) | static_cast<u8>(bytes[at+i]);
        return value;
    }
    template <usize N>
    static constexpr void put(std::array<std::byte, N> &bytes, usize at, u64 value, usize count)
    {
        for (usize i = 0; i < count; ++i) bytes[at+i] = static_cast<std::byte>(value >> (8*(count-1-i)));
    }
    static u64 backend_get(const Frame &frame, usize offset, usize count)
    {
        u64 value{};
        for (usize i = 0; i < count; ++i) value = (value << 8) | frame.fp_state[offset+i];
        return value;
    }
    static void backend_put(Frame &frame, usize offset, u64 value, usize count)
    {
        for (usize i = 0; i < count; ++i) frame.fp_state[offset+i] = static_cast<u8>(value >> (8*(count-1-i)));
    }
    static bool supported(const Frame &frame, Contract contract)
    {
        if (contract.hardware != Hardware::native32_be_e500v2_eager_spe_only ||
            (contract.resume != Resume::normalized_no_restart &&
             contract.resume != Resume::prepared_syscall_restart) || frame.flags > word_max ||
            (frame.flags & (unsupported_msr | msr_spe | msr_pr)) != (msr_spe | msr_pr) ||
            frame.pc > word_max || frame.sp > word_max || frame.fs_base > word_max || frame.gs_base > word_max ||
            (backend_get(frame, 264, 4) & spefscr_reserved)) return false;
        for (auto reg : frame.registers) if (reg > word_max) return false;
        return true;
    }
    static constexpr bool place(u64 selected_top, u64 begin, u64 end, Placement &out)
    {
        if (!window(begin, end) || selected_top <= begin || selected_top > end ||
            selected_top - begin < frame_bytes) return false;
        const u64 frame_address = (selected_top - frame_bytes) & ~u64{15};
        if (frame_address < begin || frame_address - begin < handler_gap) return false;
        const u64 sp = frame_address - handler_gap;
        if (!range(sp, allocation_bytes, begin, end)) return false;
        out = {frame_address, sp};
        return true;
    }
    static bool placement(Placement p, u64 begin, u64 end)
    {
        return !(p.stack_pointer & 15) && range(p.stack_pointer, allocation_bytes, begin, end) &&
            p.frame_address == p.stack_pointer + handler_gap;
    }
    static u64 scalar(const Frame &frame, usize reg)
    {
        return reg == 1 ? frame.sp : reg == 2 ? frame.fs_base : reg == 13 ? frame.gs_base : frame.registers[reg];
    }
    static bool encode(const Frame &frame, Capture capture, SignalMask mask, Stack stack, Info info,
                       Placement p, u64 handler, u64 begin, u64 end, Image &out)
    {
        // Interrupted SP may be invalid; a valid alternate stack can let the
        // handler repair it. Capture its full native32 value in backchain/GPR1.
        if (!capture.metadata_supplied || !supported(frame, capture.contract) || !program(frame.pc, begin, end) ||
            !program(handler, begin, end) || !placement(p, begin, end) || !valid_info(info) ||
            stack.pointer > word_max || stack.size > word_max) return false;
        Image image;
        put(image.bytes, 0, frame.sp, 4);
        mask = MaskCodec::sanitize(mask);
        const usize f = handler_gap, mc = context_offset;
        if constexpr (realtime)
        {
            if (!InfoCodec::encode_into(info, image.bytes, f)) return false;
            const usize uc = f + uc_offset;
            put(image.bytes, uc+uc_stack_offset, stack.pointer, 4);
            put(image.bytes, uc+uc_stack_offset+4, stack.flags, 4);
            put(image.bytes, uc+uc_stack_offset+8, stack.size, 4);
            put(image.bytes, uc+uc_regs_offset, p.stack_pointer+mc, 4);
            const auto wire_mask = MaskCodec::encode(mask);
            for (usize i = 0; i < wire_mask.size(); ++i) image.bytes[uc+uc_mask_offset+i] = wire_mask[i];
        }
        else
        {
            put(image.bytes, f+12, mask.words[0] >> 32, 4);
            put(image.bytes, f+16, static_cast<u32>(info.signal), 4);
            put(image.bytes, f+20, handler, 4);
            put(image.bytes, f+24, mask.words[0], 4);
            put(image.bytes, f+28, p.stack_pointer+mc, 4);
        }
        for (usize i = 0; i < 32; ++i) put(image.bytes, mc+4*i, scalar(frame, i), 4);
        put(image.bytes, mc+4*pc_slot, frame.pc, 4);
        put(image.bytes, mc+4*msr_slot, frame.flags, 4);
        put(image.bytes, mc+4*orig_r3_slot, capture.metadata.original_r3, 4);
        put(image.bytes, mc+4*ctr_slot, backend_get(frame, 276, 4), 4);
        put(image.bytes, mc+4*link_slot, backend_get(frame, 272, 4), 4);
        put(image.bytes, mc+4*xer_slot, backend_get(frame, 284, 4), 4);
        put(image.bytes, mc+4*cr_slot, backend_get(frame, 280, 4), 4);
        put(image.bytes, mc+4*trap_slot, capture.metadata.trap, 4);
        put(image.bytes, mc+4*dar_slot, capture.metadata.dar, 4);
        put(image.bytes, mc+4*dsisr_slot, capture.metadata.dsisr, 4);
        put(image.bytes, mc+4*result_slot, capture.metadata.result, 4);
        for (usize i = 0; i < 32; ++i) put(image.bytes, mc+spe_offset+4*i, backend_get(frame, 8*i, 4), 4);
        put(image.bytes, mc+spe_acc_offset, backend_get(frame, 256, 8), 8);
        put(image.bytes, mc+spefscr_offset, backend_get(frame, 264, 4), 4);
        // No classic FPU/FPSCR slots are sourced from the SPE register buffer.
        // All padding and unretained extension payloads are deterministically zero.
        out = image;
        return true;
    }
    template <typename Read>
    static bool decode(u64 syscall_sp, const Frame &current, Contract contract,
                       u64 begin, u64 end, Read &&read, ReturnPlan &out)
    {
        if (!supported(current, contract) || (syscall_sp & 15) ||
            !range(syscall_sp, allocation_bytes, begin, end)) return false;
        const u64 frame_address = syscall_sp + handler_gap;
        std::array<std::byte, prefix_bytes> prefix{};
        if (!read(frame_address, std::span<std::byte>{prefix})) return false;
        const usize pointer_offset = realtime ? uc_offset + uc_regs_offset : 28;
        const u64 mc_address = get(prefix, pointer_offset, 4);
        if ((mc_address & 15) || !range(mc_address, context_bytes, begin, end)) return false;
        std::array<std::byte, context_bytes> mc{};
        if (!read(mc_address, std::span<std::byte>{mc})) return false;
        const u64 pc = get(mc, 4*pc_slot, 4), sp = get(mc, 4, 4);
        const u32 msr = static_cast<u32>(get(mc, 4*msr_slot, 4));
        if (!program(pc, begin, end) || !stack_pointer(sp, begin, end) || (msr & unsupported_msr) ||
            get(mc, 4*mq_slot, 4) || (get(mc, spefscr_offset, 4) & spefscr_reserved)) return false;
        if constexpr (realtime) if (get(prefix, uc_offset+4, 4)) return false; // No transactional uc_link.
        // Linux without CONFIG_PPC_FPU_REGS ignores classic FP slots. This
        // bounded subset rejects nonzero unretained data rather than claiming it.
        for (usize i = fp_offset; i < pad_offset; ++i) if (mc[i] != std::byte{}) return false;
        for (usize i = spefscr_offset+4; i < context_bytes; ++i) if (mc[i] != std::byte{}) return false;
        for (usize i = 44*4; i < fp_offset; ++i) if (mc[i] != std::byte{}) return false;
        ReturnPlan plan;
        plan.frame = current;
        for (usize i = 0; i < 32; ++i)
        {
            plan.frame.registers[i] = get(mc, 4*i, 4);
            backend_put(plan.frame, 8*i, (msr & msr_spe) ? get(mc, spe_offset+4*i, 4) : 0, 4);
            backend_put(plan.frame, 8*i+4, plan.frame.registers[i], 4);
        }
        plan.frame.pc = pc;
        plan.frame.sp = sp;
        plan.frame.fs_base = plan.frame.registers[2];
        plan.frame.gs_base = plan.frame.registers[13];
        backend_put(plan.frame, 256, (msr & msr_spe) ? get(mc, spe_acc_offset, 8) : 0, 8);
        backend_put(plan.frame, 264, get(mc, spefscr_offset, 4), 4);
        backend_put(plan.frame, 272, get(mc, 4*link_slot, 4), 4);
        backend_put(plan.frame, 276, get(mc, 4*ctr_slot, 4), 4);
        backend_put(plan.frame, 280, get(mc, 4*cr_slot, 4), 4);
        backend_put(plan.frame, 284, get(mc, 4*xer_slot, 4), 4);
        // MSR and unrepresented USPRG0/backend bytes remain current/trusted.
        plan.metadata = {static_cast<u32>(get(mc, 4*orig_r3_slot, 4)),
            static_cast<u32>(get(mc, 4*trap_slot, 4)) | 1,
            static_cast<u32>(get(mc, 4*dar_slot, 4)), static_cast<u32>(get(mc, 4*dsisr_slot, 4)),
            static_cast<u32>(get(mc, 4*result_slot, 4))};
        if constexpr (realtime)
        {
            std::array<std::byte, 8> mask{};
            for (usize i = 0; i < mask.size(); ++i) mask[i] = prefix[uc_offset+uc_mask_offset+i];
            plan.mask = MaskCodec::sanitize(MaskCodec::decode(mask));
            plan.stack = {get(prefix, uc_offset+uc_stack_offset, 4), get(prefix, uc_offset+uc_stack_offset+8, 4),
                static_cast<u32>(get(prefix, uc_offset+uc_stack_offset+4, 4))};
        }
        else
        {
            plan.mask.words[0] = get(prefix, 24, 4) | (get(prefix, 12, 4) << 32);
            plan.mask = MaskCodec::sanitize(plan.mask);
        }
        out = plan;
        return true;
    }
    static bool setup_handler(const Frame &current, Contract contract, Placement p, Info info,
                              u64 handler, u64 mapped_restorer, u64 begin, u64 end, Frame &out)
    {
        if (!supported(current, contract) || !placement(p, begin, end) || !valid_info(info) ||
            !program(handler, begin, end) || !program(mapped_restorer, begin, end) ||
            !range(mapped_restorer, 8, begin, end)) return false;
        Frame frame = current;
        frame.pc = handler;
        frame.sp = p.stack_pointer;
        frame.registers[1] = p.stack_pointer;
        frame.registers[2] = frame.fs_base;
        frame.registers[13] = frame.gs_base;
        frame.registers[3] = static_cast<u32>(info.signal);
        frame.registers[4] = p.frame_address;
        if constexpr (realtime)
        {
            frame.registers[5] = p.frame_address + uc_offset;
            frame.registers[6] = p.frame_address;
        }
        backend_put(frame, 272, mapped_restorer, 4);
        // Linux chooses VDSO or an executable stack fallback, ignoring action
        // restorer here. Our caller must supply an owned RX mapped trampoline.
        out = frame;
        return true;
    }
    template <typename Write>
    static bool prepare(const Frame &current, Capture capture, SignalMask mask, Stack stack, Info info,
                        u64 selected_top, u64 handler, u64 mapped_restorer, u64 begin, u64 end,
                        Write &&write, Placement &placement_out, Frame &frame_out)
    {
        Placement p;
        Image image;
        Frame frame;
        if (!place(selected_top, begin, end, p) || !encode(current, capture, mask, stack, info, p, handler, begin, end, image) ||
            !setup_handler(current, capture.contract, p, info, handler, mapped_restorer, begin, end, frame) ||
            !write(p.stack_pointer, std::span<const std::byte>{image.bytes})) return false;
        placement_out = p;
        frame_out = frame;
        return true;
    }
};

using Ppc32LegacySignalFrame = Ppc32SignalFrame<Ppc32SignalFormat::legacy>;
using Ppc32RtSignalFrame = Ppc32SignalFrame<Ppc32SignalFormat::realtime>;

// Trusted runtime input. User MSR/SPEFSCR values cannot attest eager ownership,
// the e500v2 SPE-only profile, or normalized syscall/exception continuation.
struct Ppc32SignalContext
{
    using Hardware = Ppc32LegacySignalFrame::Hardware;
    using Resume = Ppc32LegacySignalFrame::Resume;
    using Metadata = Ppc32LegacySignalFrame::LinuxTrapMetadata;
    Hardware hardware{Hardware::unverified};
    Resume resume{Resume::unverified};
    Metadata metadata{};
    bool metadata_supplied{};

    template <Ppc32SignalFormat Format>
    [[nodiscard]] constexpr typename Ppc32SignalFrame<Format>::Capture codec() const
    {
        using C = Ppc32SignalFrame<Format>;
        return {{hardware == Hardware::native32_be_e500v2_eager_spe_only
                     ? C::Hardware::native32_be_e500v2_eager_spe_only : C::Hardware::unverified,
                 resume == Resume::normalized_no_restart
                     ? C::Resume::normalized_no_restart :
                 resume == Resume::prepared_syscall_restart
                     ? C::Resume::prepared_syscall_restart : C::Resume::unverified},
                {metadata.original_r3, metadata.trap, metadata.dar, metadata.dsisr, metadata.result},
                metadata_supplied};
    }
};

struct Ppc32SignalFramePolicy
{
    using Frame = arch::execution::UserFrame;
    using Trampoline = SignalTrampoline<abi::linux_user::Profile::ppc>;

    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &) { return true; }

    template <Ppc32SignalFormat Format, typename Write>
        requires requires(Write &write, u64 address, std::span<const std::byte> bytes) {
            { write(address, bytes) } -> std::same_as<bool>;
        }
    static bool prepare_kind(const Frame &frame, sched::SignalMask blocked, sched::PendingSignalInfo info,
                             const sched::SignalAction &action, const sched::SignalAltstack &stack,
                             u64 begin, u64 end, const Ppc32SignalContext &context, Write &&write,
                             Frame &out, u64 mapped_trampoline)
    {
        using C = Ppc32SignalFrame<Format>;
        const auto captured = context.template codec<Format>();
        if (!captured.metadata_supplied || !C::supported(frame, captured.contract)) return false;
        const bool entering = (action.flags & 0x08000000) && stack.status(frame.sp) == 0;
        const bool nested = stack.on_stack(frame.sp);
        u64 top = frame.sp;
        if (entering)
        {
            if (stack.base >= end || stack.size > end - stack.base) return false;
            top = stack.base + stack.size;
        }
        typename C::Placement placement;
        if (!C::place(top, begin, end, placement)) return false;
        // Contain the lower handler frame, saved context and untouched upper
        // ABI gap. Linux get_sigframe chooses sigsp, then rounds down to16.
        if ((entering || nested) &&
            (placement.stack_pointer < stack.base || stack.base >= end || stack.size > end - stack.base ||
             placement.stack_pointer > stack.base + stack.size ||
             C::allocation_bytes > stack.base + stack.size - placement.stack_pointer)) return false;
        u64 restorer{};
        constexpr auto entry = C::realtime ? SignalTrampolineEntry::realtime : SignalTrampolineEntry::legacy;
        if (!Trampoline::entry_address(mapped_trampoline, entry, begin, end, restorer)) return false;
        const typename C::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        typename C::Image image;
        Frame staged;
        // Native PPC ignores the action's SA_RESTORER address. Only the owned,
        // fully protected dual-entry mapping supplied by the caller is used.
        if (!C::encode(frame, captured, blocked, {stack.base, stack.size, stack.flags}, sender,
                       placement, action.handler, begin, end, image) ||
            !C::setup_handler(frame, captured.contract, placement, sender, action.handler, restorer,
                              begin, end, staged) ||
            !write(placement.stack_pointer, std::span<const std::byte>{image.bytes})) return false;
        out = staged;
        return true;
    }

    template <typename Write>
    static bool prepare(const Frame &frame, sched::SignalMask blocked, sched::PendingSignalInfo info,
                        const sched::SignalAction &action, const sched::SignalAltstack &stack,
                        u64 begin, u64 end, const Ppc32SignalContext &context, Write &&write,
                        Frame &out, u64 mapped_trampoline)
    {
        if (action.flags & 4)
            return prepare_kind<Ppc32SignalFormat::realtime>(frame, blocked, info, action, stack,
                begin, end, context, write, out, mapped_trampoline);
        return prepare_kind<Ppc32SignalFormat::legacy>(frame, blocked, info, action, stack,
            begin, end, context, write, out, mapped_trampoline);
    }
};

using Ppc32SignalDelivery = SignalDelivery<abi::linux_user::Profile::ppc, Ppc32SignalFramePolicy>;

struct Ppc32SignalReturn
{
    using Frame = arch::execution::UserFrame;
    using Operation = abi::linux_user::Operation;
    using Metadata = Ppc32SignalContext::Metadata;
    static constexpr bool restore_all_no_restart = true;
    static constexpr bool checked_user_patch_required = false;
    static constexpr bool forced_fp_signal_required = false;
    struct Restored
    {
        Frame frame{};
        sched::SignalMask mask{};
        sched::SignalAltstack stack{};
        // Untrusted ABI diagnostics; never privileged trap dispatch authority.
        Metadata metadata{};
        bool restore_stack{};
    };

    template <Ppc32SignalFormat Format, typename Read, typename CanEnter>
        requires requires(Read &read, CanEnter &admit, u64 address, std::span<std::byte> bytes, const Frame &frame) {
            { read(address, bytes) } -> std::same_as<bool>;
            { admit(frame) } -> std::same_as<bool>;
        }
    static bool decode_kind(const Frame &frame, const Ppc32SignalContext &context, u64 begin, u64 end,
                            Read &&read, CanEnter &&can_enter, Restored &out)
    {
        using C = Ppc32SignalFrame<Format>;
        const auto captured = context.template codec<Format>();
        typename C::ReturnPlan plan;
        if (!captured.metadata_supplied || !C::decode(frame.sp, frame, captured.contract, begin, end, read, plan) ||
            !can_enter(plan.frame)) return false;
        const auto &meta = plan.metadata;
        out = {plan.frame, plan.mask, {plan.stack.pointer, plan.stack.size, plan.stack.flags},
               {meta.original_r3, meta.trap, meta.dar, meta.dsisr, meta.result}, plan.restore_altstack};
        return true;
    }

    template <typename Read, typename CanEnter>
    static bool decode(Operation operation, const Frame &frame, const Ppc32SignalContext &context,
                       u64 begin, u64 end, Read &&read, CanEnter &&can_enter, Restored &out)
    {
        if (operation == Operation::sigreturn)
            return decode_kind<Ppc32SignalFormat::legacy>(frame, context, begin, end, read, can_enter, out);
        if (operation == Operation::rt_sigreturn)
            return decode_kind<Ppc32SignalFormat::realtime>(frame, context, begin, end, read, can_enter, out);
        return false;
    }

    // The first read uses current SP; the second follows the editable native
    // context pointer. Callbacks serialize/stabilize copies and cannot mutate
    // these owners. Runtime must suppress ordinary r3/CR0.SO completion, own
    // eager SPE capture, and terminate a rejected return instead of resuming it.
    template <typename Read, typename CanEnter, typename SizeValid>
        requires requires(SizeValid &valid, u64 size) { { valid(size) } noexcept -> std::same_as<bool>; }
    static bool restore(Operation operation, sched::SignalMask &blocked, Frame &frame,
                        sched::SignalAltstack &stack, Metadata &diagnostics,
                        const Ppc32SignalContext &context, u64 begin, u64 end, Read &&read,
                        CanEnter &&can_enter, SizeValid &&size_valid)
    {
        Restored staged;
        if (!decode(operation, frame, context, begin, end, read, can_enter, staged)) return false;
        auto next_stack = stack;
        if (staged.restore_stack)
            apply_restored_altstack<abi::linux_user::Profile::ppc>(next_stack, staged.stack, staged.frame.sp, size_valid);
        // No user patch or forced pending-SPEFSCR signal exists in this native
        // Linux SPE path. Every fallible copy/admission precedes publication.
        frame = staged.frame;
        blocked = staged.mask;
        stack = next_stack;
        diagnostics = staged.metadata;
        return true;
    }
};

// Metadata eligibility under the measured CPU and explicit ABI contract. This
// token neither loads the ELF nor makes an initial or cloned frame ready.
class Ppc32ImageAdmission final
{
    arch::execution::ppc32_private::Capability cpu_{};
    ppc32_elf_admission::Personality personality_{};
    bool admitted_{};
  public:
    [[nodiscard]] static Ppc32ImageAdmission inspect(
        arch::execution::ppc32_private::Capability cpu,std::span<const std::byte> prefix,u64 file_bytes,
        ppc32_elf_admission::Personality personality,ppc32_elf_admission::RuntimeProfile profile) noexcept
    {
        Ppc32ImageAdmission out;
        if(!cpu.current() || !ppc32_elf_admission::admit(prefix,file_bytes,personality,profile).ok()) return out;
        out.cpu_=cpu;out.personality_=personality;out.admitted_=true;return out;
    }
    [[nodiscard]] bool valid(arch::execution::ppc32_private::Capability cpu) const noexcept
    {return admitted_ && cpu.admitted() && cpu_.admitted() && cpu.same_issuer(cpu_);}
    [[nodiscard]] ppc32_elf_admission::Personality personality() const noexcept {return personality_;}
};

// A conservative single-native-entry proof window. Any subsequent native entry
// revokes the proof, including unrelated entries. Runtime wiring may not infer
// handler/exec/fork readiness from this object's prior state or frame shape.
class Ppc32SignalReadiness final
{
  public:
    using Frame=arch::execution::UserFrame;
    using Space=arch::execution::AddressSpace;
    using View=arch::execution::KernelEntryStackView;
    using Capability=arch::execution::ppc32_private::Capability;
    using Observation=arch::execution::ppc32_private::Observation;
    using Diagnostics=arch::execution::ppc32_private::Diagnostics;
    struct Identity
    {
        Space space{};
        u64 pid{},tid{},image_generation{};
        [[nodiscard]] constexpr bool valid() const noexcept {return space.root && space.generation && pid && tid && image_generation;}
        [[nodiscard]] constexpr bool operator==(Identity other) const noexcept
        {return arch::execution::ppc32_private::same_space(space,other.space) && pid==other.pid && tid==other.tid && image_generation==other.image_generation;}
    };
    enum class State : u8 { unverified,awaiting_capture,syscall_pending,captured,completed };
  private:
    Identity identity_{};
    Ppc32ImageAdmission image_{};
    State state_{};
    const Frame *frame_{};
    Frame saved_{};
    View view_{};
    u64 sequence_{};
    Diagnostics diagnostics_{};
    [[nodiscard]] bool owns(Identity id,Capability cap) const noexcept
    {return id.valid() && id==identity_ && image_.valid(cap) && cap.current() && arch::execution::ppc32_private::address_space_live(id.space);}
    [[nodiscard]] bool unchanged(Identity id,const Frame &frame,Capability cap) const noexcept
    {
        return owns(id,cap) && sequence_==cap.sequence() && frame_==&frame &&
            arch::execution::Ppc32PrivateViewAccess::retained(view_) &&
            arch::execution::ppc32_private::event_mode(frame,diagnostics_) &&
            arch::execution::ppc32_private::same_frame(saved_,frame);
    }
  public:
    constexpr Ppc32SignalReadiness() noexcept=default;
    Ppc32SignalReadiness(const Ppc32SignalReadiness &)=delete;
    Ppc32SignalReadiness &operator=(const Ppc32SignalReadiness &)=delete;
    [[nodiscard]] State state() const noexcept {return state_;}
    void revoke() noexcept {state_=State::unverified;frame_=nullptr;view_={};sequence_=0;diagnostics_={};}
    // Use for create/reuse/exec/fork even when a child shares an address space.
    // A copied ELF admission is eligibility only: every child needs a new trap.
    void reset(Identity id,Ppc32ImageAdmission image) noexcept {revoke();identity_=id.valid()?id:Identity{};image_=image;}
    [[nodiscard]] bool begin_entry(Identity id,const Frame &frame,Space space,const View &view,Capability cap) noexcept
    {
        revoke();
        if(!owns(id,cap) || !view.valid() || view.owner_pid()!=id.pid || view.owner_tid()!=id.tid ||
            !arch::execution::ppc32_private::same_space(space,id.space)) return false;
        state_=State::awaiting_capture;frame_=&frame;view_=view;sequence_=cap.sequence();return true;
    }
    // Must run while the exact activation remains live, before pool deactivation.
    [[nodiscard]] bool capture(Identity id,const Frame &frame,Space space,const View &view,Observation observation,Capability cap) noexcept
    {
        const bool valid=owns(id,cap) && state_==State::awaiting_capture && frame_==&frame &&
            arch::execution::Ppc32PrivateViewAccess::same(view_,view) && view.owner_pid()==id.pid && view.owner_tid()==id.tid &&
            arch::execution::ppc32_private::same_space(space,id.space) && observation.from(cap) &&
            observation.sequence()>sequence_ && observation.valid_for(frame,space,view);
        if(!valid) {revoke();return false;}
        diagnostics_=observation.diagnostics(frame,space,view);saved_=frame;sequence_=observation.sequence();
        state_=diagnostics_.event==arch::execution::ppc32_private::Event::syscall?State::syscall_pending:State::captured;
        return true;
    }
    [[nodiscard]] bool ready(Identity id,const Frame &frame,Capability cap) const noexcept
    {return (state_==State::captured || state_==State::completed) && unchanged(id,frame,cap);}
    [[nodiscard]] bool diagnostics(Identity id,const Frame &frame,Capability cap,Diagnostics &out) const noexcept
    {
        out={};
        if((state_!=State::syscall_pending && state_!=State::captured && state_!=State::completed) || !unchanged(id,frame,cap)) return false;
        out=diagnostics_;return true;
    }
    // Capture the semantic signed native32 result before the Linux bridge
    // encodes positive errno/CR0.SO. No caller-mutated frame is republished.
    [[nodiscard]] bool complete_syscall(Identity id,Frame &frame,Capability cap,abi::linux_user::Return result) noexcept
    {
        if(state_!=State::syscall_pending || !unchanged(id,frame,cap) ||
            image_.personality()!=ppc32_elf_admission::Personality::linux_user ||
            diagnostics_.event!=arch::execution::ppc32_private::Event::syscall ||
            diagnostics_.validity!=arch::execution::ppc32_private::original_r3_valid) return false;
        const u32 raw_result=result.error?u32{0}-result.error:static_cast<u32>(result.value);
        Frame completed=frame;
        if(!abi::linux_user::NativeBridge<abi::linux_user::Profile::ppc>::encode_return(completed,result).ok()) return false;
        diagnostics_.result=raw_result;diagnostics_.validity|=arch::execution::ppc32_private::result_valid;
        frame=completed;saved_=completed;state_=State::completed;return true;
    }
    [[nodiscard]] Ppc32SignalContext context(Identity id,const Frame &frame,Capability cap) const noexcept
    {
        Ppc32SignalContext out{};
        if(!ready(id,frame,cap) || image_.personality()!=ppc32_elf_admission::Personality::linux_user) return out;
        out.hardware=Ppc32SignalContext::Hardware::native32_be_e500v2_eager_spe_only;
        out.resume=Ppc32SignalContext::Resume::normalized_no_restart;
        out.metadata={diagnostics_.original_r3,diagnostics_.trap,diagnostics_.dar,diagnostics_.dsisr,diagnostics_.result};
        out.metadata_supplied=true;return out;
    }
};

template <abi::linux_user::Profile> class SyscallRestartPublication;
// Eligibility for one canonical pool record. It does not confer an event origin.
class Ppc32OwnedLease final
{
    friend class SyscallRestartPublication<abi::linux_user::Profile::ppc>;
    using Access=arch::execution::Ppc32PrivateViewAccess;
    memory::KernelEntryStackPool *pool_{};
    memory::KernelEntryStackHandle handle_{};
    memory::KernelEntryStackOwner owner_{};
    Access::RecordSnapshot pinned_{};
    [[nodiscard]] Access::RecordSnapshot current() const noexcept
    {return pool_?Access::inspect_record(*pool_,handle_.slot):Access::RecordSnapshot{};}
    [[nodiscard]] bool matches(Access::RecordSnapshot r) const noexcept
    {return pool_ && handle_.pool==pool_ && r.record && r.record==pinned_.record &&
        r.generation==handle_.generation && r.pid==owner_.pid && r.tid==owner_.tid &&
        r.lower==pinned_.lower && r.top==pinned_.top;}
  public:
    [[nodiscard]] static Ppc32OwnedLease inspect(memory::KernelEntryStackPool &pool,
        memory::KernelEntryStackHandle handle,memory::KernelEntryStackOwner owner) noexcept
    {
        Ppc32OwnedLease out;
        if(!pool.validate_inactive(handle,owner)) return out;
        const auto record=Access::inspect_record(pool,handle.slot);
        if(!record.record || !record.inactive || record.generation!=handle.generation ||
            record.pid!=owner.pid || record.tid!=owner.tid) return out;
        out.pool_=&pool;out.handle_=handle;out.owner_=owner;out.pinned_=record;return out;
    }
    [[nodiscard]] bool inactive() const noexcept
    {
        return pool_ && pool_->validate_inactive(handle_,owner_).ok() &&
            matches(current()) && current().inactive;
    }
    [[nodiscard]] bool retained(u64 epoch) const noexcept
    {return inactive() && current().epoch==epoch;}
    [[nodiscard]] bool active(const arch::execution::KernelEntryStackView &view) const noexcept
    {const auto r=current();return matches(r) && Access::matches_record(view,r);}
    [[nodiscard]] u64 epoch() const noexcept {const auto r=current();return matches(r)?r.epoch:0;}
    [[nodiscard]] memory::KernelEntryStackOwner owner() const noexcept {return owner_;}
    [[nodiscard]] bool same_record(const Ppc32OwnedLease &other) const noexcept
    {return pool_ && pool_==other.pool_ && handle_==other.handle_ && pinned_.record==other.pinned_.record;}
};

// Store one stable helper per canonical lease slot, outside movable TCB records.
// The owner cancels/revokes before partial mutation, exec, reap or lease release.
class Ppc32PersistentReadiness final
{
    friend class SyscallRestartPublication<abi::linux_user::Profile::ppc>;
  public:
    using Frame=arch::execution::UserFrame;
    using Space=arch::execution::AddressSpace;
    using View=arch::execution::KernelEntryStackView;
    using Capability=arch::execution::ppc32_private::Capability;
    using Observation=arch::execution::ppc32_private::Observation;
    using Diagnostics=arch::execution::ppc32_private::Diagnostics;
    using Identity=Ppc32SignalReadiness::Identity;
    using Return=abi::linux_user::Return;
    using Bridge=abi::linux_user::NativeBridge<abi::linux_user::Profile::ppc>;
    using Codec=abi::linux_user::Codec<abi::linux_user::Profile::ppc>;
    using Operation=abi::linux_user::Operation;
    enum class State:u8 {unverified,awaiting_capture,syscall_pending,syscall_completed,timer,fault,child,handler,sigreturn,restart_prepared};
    enum class Transaction:u8 {none,handler,sigreturn,relocation,space,restart};
    struct ChildEdits {std::optional<uptr> stack{},tls{};};
  private:
    Identity identity_{};
    Ppc32ImageAdmission image_{};
    Ppc32OwnedLease lease_{};
    State state_{};
    Transaction transaction_{};
    Capability capability_{};
    const Frame *frame_{};
    Frame saved_{},candidate_{};
    View entry_view_{};
    Diagnostics diagnostics_{};
    u64 sequence_floor_{},lease_epoch_{};
    Space next_space_{};
    bool fresh_{};
    [[nodiscard]] bool owns(Identity id,Capability cap,bool live_space=true) const noexcept
    {
        return id.valid() && id==identity_ && image_.valid(cap) && capability_.admitted() &&
            cap.admitted() && cap.same_issuer(capability_) &&
            (!live_space || arch::execution::ppc32_private::address_space_live(id.space));
    }
    [[nodiscard]] bool linux() const noexcept
    {return image_.personality()==ppc32_elf_admission::Personality::linux_user;}
    [[nodiscard]] bool has_origin() const noexcept
    {return state_!=State::unverified && state_!=State::awaiting_capture;}
    [[nodiscard]] bool signal_state() const noexcept
    {return has_origin() && state_!=State::syscall_pending;}
    [[nodiscard]] bool unchanged(Identity id,const Frame &frame,Capability cap) const noexcept
    {
        return transaction_==Transaction::none && has_origin() && owns(id,cap) &&
            lease_.retained(lease_epoch_) && frame_==&frame &&
            arch::execution::ppc32_private::same_frame(saved_,frame);
    }
    [[nodiscard]] bool shape(const Frame &frame) const noexcept
    {
        return arch::execution::ppc32_private::event_mode(frame,diagnostics_) &&
            Ppc32LegacySignalFrame::program(frame.pc,arch::execution::user_begin,arch::execution::user_end) &&
            Ppc32LegacySignalFrame::stack_pointer(frame.sp,arch::execution::user_begin,arch::execution::user_end);
    }
    [[nodiscard]] bool syscall(Operation operation) const noexcept
    {const auto number=Codec::syscall_number(operation);return number && saved_.registers[0]==*number;}
    [[nodiscard]] bool completion(Identity id,const Frame &frame,Capability cap,Return result,Frame &out) const noexcept
    {
        if(state_!=State::syscall_pending || !linux() || !owns(id,cap) ||
            transaction_!=Transaction::none || !lease_.retained(lease_epoch_) || frame_!=&frame ||
            diagnostics_.event!=arch::execution::ppc32_private::Event::syscall ||
            diagnostics_.validity!=arch::execution::ppc32_private::original_r3_valid) return false;
        // The pinned PPC adapter has no register-return pipe or TLS syscall.
        static_assert(!Codec::syscall_number(Operation::set_thread_area));
        static_assert(!Codec::syscall_number(Operation::arch_prctl));
        static_assert(!Codec::syscall_number(Operation::arm_set_tls));
        out=saved_;return Bridge::encode_return(out,result).ok();
    }
    void remember(const Frame &frame,State state) noexcept
    {frame_=&frame;saved_=frame;state_=state;transaction_=Transaction::none;fresh_=false;}
    void completed(const Frame &frame,Return result) noexcept
    {
        diagnostics_.result=result.error?u32{0}-result.error:static_cast<u32>(result.value);
        diagnostics_.validity|=arch::execution::ppc32_private::result_valid;
        remember(frame,State::syscall_completed);
    }
    [[nodiscard]] bool stage_policy(Identity id,const Frame &before,const Frame &after,Capability cap,Transaction kind) noexcept
    {
        if(!unchanged(id,before,cap) || !linux() || !shape(after)) return false;
        if(kind==Transaction::handler ? !signal_state() :
            (state_!=State::syscall_pending || (!syscall(Operation::sigreturn) && !syscall(Operation::rt_sigreturn)))) return false;
        candidate_=after;transaction_=kind;return true;
    }
    [[nodiscard]] bool publish_policy(Identity id,const Frame &after,Capability cap,Transaction kind,State state) noexcept
    {
        const bool valid=transaction_==kind && owns(id,cap) && lease_.retained(lease_epoch_) && frame_==&after &&
            arch::execution::ppc32_private::same_frame(candidate_,after) && shape(after);
        if(!valid){revoke();return false;}
        remember(after,state);return true;
    }
  public:
    constexpr Ppc32PersistentReadiness() noexcept=default;
    Ppc32PersistentReadiness(const Ppc32PersistentReadiness &)=delete;
    Ppc32PersistentReadiness &operator=(const Ppc32PersistentReadiness &)=delete;
    [[nodiscard]] Identity identity() const noexcept {return identity_;}
    [[nodiscard]] State state() const noexcept {return state_;}
    [[nodiscard]] Transaction transaction() const noexcept {return transaction_;}
    void revoke() noexcept
    {state_=State::unverified;transaction_=Transaction::none;frame_=nullptr;entry_view_={};diagnostics_={};fresh_=false;next_space_={};}
    [[nodiscard]] bool reset(Identity id,Ppc32ImageAdmission image,Ppc32OwnedLease lease,Capability cap) noexcept
    {
        const bool new_binding=!(id==identity_) || !lease.same_record(lease_);
        // Failed admission revokes authority but retains the previous binding
        // tombstone, so retrying that binding cannot rearm child derivation.
        revoke();image_={};capability_={};
        if(!id.valid() || !image.valid(cap) || !cap.admitted() || !lease.inactive() ||
            lease.owner()!=memory::KernelEntryStackOwner{id.pid,id.tid} ||
            !arch::execution::ppc32_private::address_space_live(id.space)) return false;
        identity_=id;image_=image;lease_=lease;capability_=cap;lease_epoch_=lease.epoch();fresh_=new_binding;return true;
    }
    [[nodiscard]] bool begin_entry(Identity id,const Frame &frame,Space space,const View &view,Capability cap) noexcept
    {
        revoke();
        if(!owns(id,cap) || !cap.current() || !lease_.active(view) ||
            !arch::execution::ppc32_private::same_space(space,id.space)) return false;
        frame_=&frame;entry_view_=view;sequence_floor_=cap.sequence();state_=State::awaiting_capture;return true;
    }
    [[nodiscard]] bool capture(Identity id,const Frame &frame,Space space,const View &view,Observation observation,Capability cap) noexcept
    {
        const bool valid=state_==State::awaiting_capture && owns(id,cap) && frame_==&frame &&
            lease_.active(view) && arch::execution::Ppc32PrivateViewAccess::same(entry_view_,view) &&
            arch::execution::ppc32_private::same_space(space,id.space) && observation.from(cap) &&
            observation.sequence()>sequence_floor_ && observation.valid_for(frame,space,view);
        if(!valid){revoke();return false;}
        diagnostics_=observation.diagnostics(frame,space,view);lease_epoch_=lease_.epoch();sequence_floor_=observation.sequence();
        using Event=arch::execution::ppc32_private::Event;
        remember(frame,diagnostics_.event==Event::syscall?State::syscall_pending:
            diagnostics_.event==Event::timer?State::timer:State::fault);entry_view_={};return true;
    }
    [[nodiscard]] bool ready(Identity id,const Frame &frame,Capability cap) const noexcept
    {return signal_state() && unchanged(id,frame,cap);}
    [[nodiscard]] bool pending(Identity id,const Frame &frame,Capability cap) const noexcept
    {return state_==State::syscall_pending && unchanged(id,frame,cap);}
    [[nodiscard]] bool diagnostics(Identity id,const Frame &frame,Capability cap,Diagnostics &out) const noexcept
    {out={};if(!unchanged(id,frame,cap)) return false;out=diagnostics_;return true;}
    [[nodiscard]] bool complete_syscall(Identity id,Frame &frame,Capability cap,Return result) noexcept
    {
        Frame next;
        if(!unchanged(id,frame,cap) || !completion(id,frame,cap,result,next)) return false;
        frame=next;completed(frame,result);return true;
    }
    // Call only after the ordinary adapter committed precisely its return fields.
    [[nodiscard]] bool publish_linux_completion(Identity id,const Frame &frame,Capability cap,Return result) noexcept
    {
        Frame expected;
        if(!completion(id,frame,cap,result,expected) || !arch::execution::ppc32_private::same_frame(expected,frame)) return false;
        completed(frame,result);return true;
    }
    [[nodiscard]] bool inherit_syscall(Identity id,const Ppc32PersistentReadiness &parent,Identity parent_id,
        const Frame &parent_frame,const Frame &child,ChildEdits edits,Capability cap) noexcept
    {
        if(!fresh_ || state_!=State::unverified || transaction_!=Transaction::none || !owns(id,cap) || !linux() ||
            !lease_.retained(lease_epoch_) || lease_.same_record(parent.lease_) || id.tid==parent_id.tid || &child==&parent_frame ||
            !parent.linux() || !parent.unchanged(parent_id,parent_frame,cap) ||
            (parent.state_!=State::syscall_pending && parent.state_!=State::syscall_completed) ||
            (!parent.syscall(Operation::fork) && !parent.syscall(Operation::clone))) return false;
        if(parent.syscall(Operation::fork) && (edits.stack || edits.tls)) return false;
        Frame expected=parent_frame;
        if(edits.stack) Bridge::set_stack_pointer(expected,*edits.stack);
        if(edits.tls && !Bridge::set_thread_pointer(expected,{*edits.tls}).ok()) return false;
        if(!Bridge::encode_return(expected,{0,0}).ok() || !arch::execution::ppc32_private::same_frame(expected,child)) return false;
        diagnostics_=parent.diagnostics_;
        if(!shape(child)){diagnostics_={};return false;}
        // Ancestry is retained, but no child syscall completion authority exists.
        remember(child,State::child);return true;
    }
    [[nodiscard]] bool stage_handler(Identity id,const Frame &before,const Frame &candidate,Capability cap) noexcept
    {return stage_policy(id,before,candidate,cap,Transaction::handler);}
    [[nodiscard]] bool publish_handler(Identity id,const Frame &after,Capability cap) noexcept
    {return publish_policy(id,after,cap,Transaction::handler,State::handler);}
    [[nodiscard]] bool stage_sigreturn(Identity id,const Frame &before,const Frame &candidate,Capability cap) noexcept
    {return stage_policy(id,before,candidate,cap,Transaction::sigreturn);}
    [[nodiscard]] bool publish_sigreturn(Identity id,const Frame &after,Capability cap) noexcept
    {return publish_policy(id,after,cap,Transaction::sigreturn,State::sigreturn);}
    [[nodiscard]] bool stage_relocation(Identity id,const Frame &before,Capability cap) noexcept
    {
        if(!unchanged(id,before,cap)) return false;
        transaction_=Transaction::relocation;return true;
    }
    [[nodiscard]] bool commit_relocation(Identity id,const Frame &after,Capability cap) noexcept
    {
        const bool valid=transaction_==Transaction::relocation && owns(id,cap) && lease_.retained(lease_epoch_) &&
            arch::execution::ppc32_private::same_frame(saved_,after);
        if(!valid){revoke();return false;}
        frame_=&after;transaction_=Transaction::none;return true;
    }
    [[nodiscard]] bool stage_space(Identity id,const Frame &frame,Space next,Capability cap) noexcept
    {
        if(!unchanged(id,frame,cap) || !next.root || !next.generation || arch::execution::ppc32_private::same_space(next,id.space)) return false;
        next_space_=next;transaction_=Transaction::space;return true;
    }
    // The old root may already have been retired after the successful stage.
    [[nodiscard]] bool commit_space(Identity before,Identity after,const Frame &frame,Capability cap) noexcept
    {
        auto expected=before;expected.space=next_space_;
        const bool valid=transaction_==Transaction::space && owns(before,cap,false) && after.valid() && after==expected &&
            lease_.retained(lease_epoch_) && frame_==&frame && arch::execution::ppc32_private::same_frame(saved_,frame) &&
            arch::execution::ppc32_private::address_space_live(after.space);
        if(!valid){revoke();return false;}
        identity_=after;transaction_=Transaction::none;next_space_={};return true;
    }
    // A canceled staged operation may already have changed or retired its source.
    void cancel() noexcept {revoke();}
    [[nodiscard]] Ppc32SignalContext context(Identity id,const Frame &frame,Capability cap) const noexcept
    {
        Ppc32SignalContext out{};if(!linux() || !ready(id,frame,cap)) return out;
        out.hardware=Ppc32SignalContext::Hardware::native32_be_e500v2_eager_spe_only;
        out.resume=state_==State::restart_prepared ? Ppc32SignalContext::Resume::prepared_syscall_restart
            : Ppc32SignalContext::Resume::normalized_no_restart;
        out.metadata={diagnostics_.original_r3,diagnostics_.trap,diagnostics_.dar,diagnostics_.dsisr,diagnostics_.result};
        out.metadata_supplied=true;return out;
    }
};
} // namespace ok::user::native
