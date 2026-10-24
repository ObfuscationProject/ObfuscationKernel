#pragma once
#include "ok/arch/mips64_n64_private_signal.hpp"
#include "ok/user/native_signal_frame_mips64_n64.hpp"

namespace ok::user::native
{
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
} // namespace ok::user::native
