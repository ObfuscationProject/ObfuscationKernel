#pragma once

#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"

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

} // namespace ok::user::native
