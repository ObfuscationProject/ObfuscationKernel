#pragma once
// MIPS64 n64 native signal frames, delivery and return decoding, with the private readiness state.
#include "ok/arch/execution.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_signal_info.hpp"
#include "ok/user/native_signal_delivery.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#include "ok/user/native_signal_altstack.hpp"
#include "ok/user/native_signal_force.hpp"
#include "ok/arch/mips64_n64_private_signal.hpp"

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

struct Mips64N64SignalFramePolicy
{
    using Frame = Mips64N64SignalFrame;
    using Trampoline = SignalTrampoline<abi::linux_user::Profile::mips64>;
    [[nodiscard]] static constexpr bool supported(const sched::SignalAction &) { return true; }

    template <typename Write>
    static bool prepare(const arch::execution::UserFrame &frame, sched::SignalMask blocked,
                        sched::PendingSignalInfo info, const sched::SignalAction &action,
                        const sched::SignalAltstack &stack, u64 begin, u64 end,
                        const Frame::Capture &context, Write &&write,
                        arch::execution::UserFrame &out, u64 mapped_trampoline)
    {
        if (!Frame::capture(frame, context)) return false;
        const bool nested = stack.on_stack(frame.sp);
        if (nested && !stack.on_stack(frame.sp - Frame::frame_bytes)) return false;
        // Native n64 subtraction is 64-bit. Do not use the o32 truncation here.
        const u64 after_gap = frame.sp - 32;
        const bool entering = (action.flags & 0x08000000) && stack.status(after_gap) == 0;
        u64 top = frame.sp;
        if (entering)
        {
            if (stack.base >= end || stack.size > end - stack.base) return false;
            top = stack.base + stack.size;
        }
        Frame::Placement placement;
        if (!Frame::place(top, entering, begin, end, placement)) return false;
        // Contain the aligned/gapped placement as well as Linux's pre-gap check.
        if ((nested || entering || stack.on_stack(after_gap)) && !stack.physically_contains(placement.address))
            return false;
        u64 restorer{};
        if (!Trampoline::entry_address(mapped_trampoline, SignalTrampolineEntry::realtime,
                                        begin, end, restorer)) return false;
        const Frame::Info sender{static_cast<i32>(info.signal), info.code, info.sender_pid, info.sender_uid, info.child_status};
        Frame::Image image;
        arch::execution::UserFrame next;
        if (!Frame::encode(frame, context, blocked, {stack.base, stack.size, stack.flags}, sender, begin, end, image) ||
            !Frame::setup_handler(frame, context, placement, sender, action.handler, restorer, begin, end, next))
            return false;
        if (!write(placement.address, std::span<const std::byte>{image.bytes})) return false;
        out = next;
        return true;
    }
};

using Mips64N64SignalDelivery = SignalDelivery<abi::linux_user::Profile::mips64, Mips64N64SignalFramePolicy>;

struct Mips64N64SignalReturn
{
    using Codec = Mips64N64SignalFrame;
    using Frame = arch::execution::UserFrame;
    using Operation = abi::linux_user::Operation;

    template <typename Queue, typename Read, typename Write, typename CanEnter, typename SizeValid>
    static bool restore(Operation operation, Queue &pending, sched::SignalActionTable &actions,
                        sched::SignalMask &blocked, Frame &frame, sched::SignalAltstack &stack,
                        const Codec::Capture &context, u64 begin, u64 end,
                        Read &&read, Write &&write, CanEnter &&can_enter, SizeValid &&size_valid)
    {
        // Native n64 has only RT return, regardless of the action's SIGINFO flag.
        if (operation != Operation::rt_sigreturn) return false;
        Codec::ReturnPlan staged;
        if (!Codec::decode(frame.sp, frame, context, begin, end, read, staged) ||
            !can_enter(staged.frame)) return false;
        auto next_stack = stack;
        apply_restored_altstack<abi::linux_user::Profile::mips64>(next_stack,
            {staged.stack.pointer, staged.stack.size, staged.stack.flags}, staged.frame.sp, size_valid);
        if (staged.fcsr_patch.required &&
            !write(staged.fcsr_patch.address, std::span<const std::byte>{staged.fcsr_patch.bytes})) return false;
        if (staged.pending == Codec::Pending::sigfpe && !force_kernel_signal(pending, actions, staged.mask, 8))
            return false;
        frame = staged.frame;
        blocked = staged.mask;
        stack = next_stack;
        // Runtime suppresses ordinary completion to preserve restored v0/a3.
        return true;
    }
};

template <abi::linux_user::Profile> class SyscallRestartPublication;
class Mips64N64SignalReadiness final
{
    friend class SyscallRestartPublication<abi::linux_user::Profile::mips64>;
  public:
    using Backend = arch::execution::UserFrame;
    using Capability = arch::execution::mips64_n64_private::Capability;
    using Observation = arch::execution::mips64_n64_private::Observation;
    using Space = arch::execution::AddressSpace;
    using View = arch::execution::KernelEntryStackView;
    struct Identity
    {
        uptr pool{}, lower{}, top{};
        u32 slot{};
        u64 pid{}, tid{}, lease_generation{}, image_generation{};
        Space space{};
        [[nodiscard]] constexpr bool valid() const noexcept
        { return pool && lower && top>lower && !(lower&15) && !(top&15) && top-lower>=4096 &&
            pid && tid && lease_generation && image_generation && space.generation; }
        [[nodiscard]] constexpr bool operator==(const Identity &other) const noexcept
        { return pool==other.pool && lower==other.lower && top==other.top && slot==other.slot &&
            pid==other.pid && tid==other.tid && lease_generation==other.lease_generation &&
            image_generation==other.image_generation &&
            arch::execution::mips64_n64_private::same_space(space,other.space); }
    };
    enum class State : u8
    { unverified, initial, awaiting_capture, syscall_normalized, timer_non_bd, timer_bd, handler, sigreturn, restart_staged, restart_prepared };
  private:
    Identity identity_{};
    State state_{};
    Capability capability_{};
    const Backend *frame_{};
    Backend saved_{};
    View entry_view_{};
    u64 sequence_floor_{};
    bool initial_allowed_{}, actual_syscall_{}, relocation_pending_{};
    [[nodiscard]] bool owns(Identity id) const noexcept {return id.valid() && id==identity_;}
    [[nodiscard]] bool capable(Capability cap) const noexcept
    { return cap.admitted() && capability_.admitted() && cap.same_issuer(capability_); }
    [[nodiscard]] static bool shape(const Backend &frame) noexcept
    {
        return arch::execution::mips64_n64_private::frame_mode(frame) &&
            Mips64N64SignalFrame::program(frame.pc,arch::execution::user_begin,arch::execution::user_end) &&
            Mips64N64SignalFrame::stack_pointer(frame.sp,arch::execution::user_begin,arch::execution::user_end) &&
            frame.registers[29]==frame.sp;
    }
    [[nodiscard]] bool unchanged(const Backend &frame) const noexcept
    { return frame_==&frame && arch::execution::mips64_n64_private::same_frame(saved_,frame); }
    void remember(const Backend &frame, State state, bool actual_syscall=false) noexcept
    { relocation_pending_=false;frame_=&frame;saved_=frame;state_=state;initial_allowed_=false;actual_syscall_=actual_syscall; }
    [[nodiscard]] bool live_view(const View &view) const noexcept
    {
        return view.valid() && arch::execution::Mips64N64PrivateViewAccess::generation(view)==identity_.lease_generation &&
            view.lower()==identity_.lower && view.top()==identity_.top &&
            view.owner_pid()==identity_.pid && view.owner_tid()==identity_.tid;
    }
  public:
    [[nodiscard]] Identity identity() const noexcept {return identity_;}
    [[nodiscard]] State state() const noexcept {return state_;}
    // Trusted creation/reuse/exec hook: the runtime supplies its current image
    // generation and the bounds obtained from its verified inactive pool lease.
    void reset(Identity id, Capability cap) noexcept
    {
        initial_allowed_=id.valid() && !(id==identity_);
        identity_=id.valid()?id:Identity{};capability_=cap;state_=State::unverified;
        sequence_floor_=cap.sequence();frame_=nullptr;entry_view_={};actual_syscall_=false;relocation_pending_=false;
    }
    void revoke(Identity id) noexcept
    {
        if(owns(id)) {state_=State::unverified;frame_=nullptr;entry_view_={};initial_allowed_=false;actual_syscall_=false;relocation_pending_=false;}
    }
    [[nodiscard]] bool ready(Identity id, const Backend &frame, Capability cap) const noexcept
    {
        return owns(id) && capable(cap) && unchanged(frame) &&
            (state_==State::initial || state_==State::syscall_normalized || state_==State::timer_non_bd ||
             state_==State::handler || state_==State::sigreturn || state_==State::restart_prepared);
    }
    [[nodiscard]] bool deferred(Identity id, const Backend &frame, Capability cap) const noexcept
    { return owns(id) && capable(cap) && unchanged(frame) && state_==State::timer_bd; }
    // Trusted scheduler compaction only. Stage while the canonical source
    // still exists; commit after its nonthrowing copy, before result encoding.
    // No observation, initial origin, or syscall origin is minted here.
    [[nodiscard]] bool stage_relocation(Identity id, const Backend &frame, Capability cap) noexcept
    {
        if(relocation_pending_ || (!ready(id,frame,cap) && !deferred(id,frame,cap))) return false;
        relocation_pending_=true;return true;
    }
    [[nodiscard]] bool commit_relocation(Identity id, const Backend &frame, Capability cap) noexcept
    {
        const bool staged=relocation_pending_;relocation_pending_=false;
        if(!staged || !owns(id) || !capable(cap) ||
            !arch::execution::mips64_n64_private::same_frame(saved_,frame)) return false;
        frame_=&frame;return true;
    }
    void cancel_relocation() noexcept { relocation_pending_=false; }
    [[nodiscard]] bool can_publish_space(Identity id, const Backend &frame, Space next, Capability cap) const noexcept
    {
        return !relocation_pending_ && next.generation &&
            !arch::execution::mips64_n64_private::same_space(next,id.space) &&
            (ready(id,frame,cap) || deferred(id,frame,cap));
    }
    // Publish a committed page-table replacement without changing the image,
    // leased stack, captured frame, or the syscall/timer/handler provenance.
    [[nodiscard]] bool publish_space(Identity before, Identity after, const Backend &frame, Capability cap) noexcept
    {
        auto expected=before;expected.space=after.space;
        if(!after.valid() || !(expected==after) || !can_publish_space(before,frame,after.space,cap)) return false;
        identity_=after;return true;
    }
    // Only after ELF, mappings, initial frame and stack admission all succeed.
    [[nodiscard]] bool publish_initial(Identity id, const Backend &frame, Capability cap) noexcept
    {
        if(!owns(id) || !initial_allowed_ || state_!=State::unverified || !capable(cap) || !shape(frame)) return false;
        remember(frame,State::initial);return true;
    }
    // Revoke even when admission fails. Call after activating the exact lease.
    [[nodiscard]] bool begin_entry(Identity id, const Backend &frame, Space space, const View &view, Capability cap) noexcept
    {
        state_=State::unverified;frame_=nullptr;entry_view_={};initial_allowed_=false;actual_syscall_=false;relocation_pending_=false;
        if(!owns(id) || !capable(cap) || !cap.current() || !live_view(view) ||
            !arch::execution::mips64_n64_private::same_space(space,identity_.space)) return false;
        state_=State::awaiting_capture;frame_=&frame;entry_view_=view;sequence_floor_=cap.sequence();return true;
    }
    // Consume while the view is active, after enter_owned and before pool
    // deactivation. An old/deactivated epoch or edited frame cannot be consumed.
    [[nodiscard]] bool capture(Identity id, const Backend &frame, Space space, const View &view,
                               Observation observation, Capability cap) noexcept
    {
        const bool valid=owns(id) && capable(cap) && state_==State::awaiting_capture && frame_==&frame &&
            arch::execution::Mips64N64PrivateViewAccess::same(entry_view_,view) && live_view(view) && observation.from(cap) &&
            observation.sequence()>sequence_floor_ && observation.valid_for(frame,space,view) &&
            arch::execution::mips64_n64_private::same_space(space,identity_.space);
        state_=State::unverified;entry_view_={};initial_allowed_=false;actual_syscall_=false;relocation_pending_=false;
        if(!valid) {frame_=nullptr;return false;}
        sequence_floor_=observation.sequence();
        using Kind=arch::execution::mips64_n64_private::CaptureKind;
        switch(observation.kind())
        {
        case Kind::syscall_normalized: remember(frame,State::syscall_normalized,true);return true;
        case Kind::timer_non_bd: remember(frame,State::timer_non_bd);return true;
        case Kind::timer_bd: remember(frame,State::timer_bd);return true;
        default: frame_=nullptr;return false;
        }
    }
    // Normal completion may change only Linux n64 v0/a3. A sigreturn transaction
    // must use publish_sigreturn instead and suppress ordinary completion.
    [[nodiscard]] bool publish_syscall_completion(Identity id, const Backend &frame, Capability cap) noexcept
    {
        if(!owns(id) || !capable(cap) || state_!=State::syscall_normalized || !actual_syscall_ || frame_!=&frame) return false;
        auto expected=saved_;expected.registers[2]=frame.registers[2];expected.registers[7]=frame.registers[7];
        if(!arch::execution::mips64_n64_private::same_frame(expected,frame)) return false;
        remember(frame,State::syscall_normalized,true);return true;
    }
    // The bounded Linux n64 adapter has exactly two additional saved-state
    // effects. Authorize them from the captured syscall and trusted result,
    // never from the edited user frame's claimed syscall number.
    [[nodiscard]] bool publish_linux_completion(Identity id, const Backend &frame, Capability cap,
                                                i64 result) noexcept
    {
        if(!owns(id) || !capable(cap) || state_!=State::syscall_normalized || !actual_syscall_ || frame_!=&frame) return false;
        using Codec=abi::linux_user::Codec<abi::linux_user::Profile::mips64>;
        using Operation=abi::linux_user::Operation;
        auto expected=saved_;
        expected.registers[2]=frame.registers[2];expected.registers[7]=frame.registers[7];
        if(saved_.registers[2]==*Codec::syscall_number(Operation::set_thread_area) && result==0)
            expected.fs_base=saved_.registers[4];
        if(saved_.registers[2]==*Codec::syscall_number(Operation::pipe) && result>=0)
        {
            if(frame.registers[3]>0x7fffffffU) return false;
            expected.registers[3]=frame.registers[3];
        }
        if(!arch::execution::mips64_n64_private::same_frame(expected,frame)) return false;
        remember(frame,State::syscall_normalized,true);return true;
    }
    // Child frame edits and its mapping/stack/TLS admission must finish first.
    // The parent must still retain an actual consumed normalized syscall proof.
    [[nodiscard]] bool inherit_syscall(Identity id, const Mips64N64SignalReadiness &parent,
        Identity parent_id, const Backend &parent_frame, const Backend &child, Capability cap) noexcept
    {
        if(!owns(id) || !initial_allowed_ || state_!=State::unverified || !capable(cap) ||
            parent.state_!=State::syscall_normalized || !parent.actual_syscall_ || !parent.ready(parent_id,parent_frame,cap) ||
            !shape(child) || child.pc!=parent_frame.pc) return false;
        // A child can resume this PC, but has not itself executed a syscall.
        remember(child,State::syscall_normalized);return true;
    }
    // Explicit post-commit hooks. The caller completes all policy writes,
    // forced-signal queueing and the in-place live-frame commit first; an
    // ordinary readiness query never infers this event from changed registers.
    [[nodiscard]] bool publish_handler(Identity id, const Backend &after, Capability cap) noexcept
    {
        const bool previous=state_==State::initial || state_==State::syscall_normalized ||
            state_==State::timer_non_bd || state_==State::handler || state_==State::sigreturn ||
            state_==State::restart_prepared;
        if(!owns(id) || !capable(cap) || !previous || frame_!=&after || !shape(after)) return false;
        remember(after,State::handler);return true;
    }
    [[nodiscard]] bool publish_sigreturn(Identity id, const Backend &after, Capability cap) noexcept
    {
        if(!owns(id) || !capable(cap) || state_!=State::syscall_normalized || !actual_syscall_ || frame_!=&after || !shape(after)) return false;
        remember(after,State::sigreturn);return true;
    }
    [[nodiscard]] Mips64N64SignalFrame::Capture context(Identity id, const Backend &frame, Capability cap) const noexcept
    {
        Mips64N64SignalFrame::Capture result{};
        if(capable(cap)) result.fp=Mips64N64SignalFrame::FpContract::eager_fr1_legacy_nan_no_extensions;
        if(ready(id,frame,cap)) result.resume=state_==State::restart_prepared
            ? Mips64N64SignalFrame::ResumeContract::prepared_syscall_restart
            : Mips64N64SignalFrame::ResumeContract::normalized_resume_pc;
        return result;
    }
};

enum class Mips64N64HandlerGate : u8 { proceed, defer_handler, unverified };
template<typename Queue>
[[nodiscard]] Mips64N64HandlerGate mips64_n64_handler_gate(
    const Queue &pending, const sched::SignalActionTable &actions, sched::SignalMask blocked,
    const Mips64N64SignalReadiness &readiness, Mips64N64SignalReadiness::Identity identity,
    const arch::execution::UserFrame &frame, Mips64N64SignalReadiness::Capability capability) noexcept
{
    typename Queue::Delivery ticket;
    if(!pending.peek(blocked,ticket)) return Mips64N64HandlerGate::proceed;
    const auto *action=actions.find(ticket.info.signal);
    using Delivery=Mips64N64SignalDelivery;
    if(!action || Delivery::Actions::ignored(ticket.info.signal,*action) ||
        action->handler==sched::SignalAction::default_handler || !Delivery::supported(*action))
        return Mips64N64HandlerGate::proceed;
    if(!capability.admitted() || !(readiness.identity()==identity)) return Mips64N64HandlerGate::unverified;
    if(readiness.ready(identity,frame,capability)) return Mips64N64HandlerGate::proceed;
    return readiness.deferred(identity,frame,capability)
        ? Mips64N64HandlerGate::defer_handler : Mips64N64HandlerGate::unverified;
}
} // namespace ok::user::native
