#pragma once
#include "ok/arch/mips_o32_private_signal.hpp"
#include "ok/user/native_signal_context_mips_o32.hpp"

namespace ok::user::native
{
class MipsO32SignalReadiness final
{
  public:
    using Capability = arch::execution::mips_o32_private::Capability;
    using Observation = arch::execution::mips_o32_private::Observation;
    using UserFrame = arch::execution::UserFrame;
    struct Identity
    {
        uptr pool{};
        u32 slot{};
        u64 pid{}, tid{}, generation{};
        [[nodiscard]] constexpr bool valid() const noexcept { return pool && pid && tid && generation; }
        constexpr bool operator==(const Identity &) const = default;
    };
    enum class State : u8
    { unverified, initial, awaiting_capture, syscall_normalized, timer_non_bd, timer_bd, handler, sigreturn };
  private:
    Identity identity_{};
    State state_{};
    u64 sequence_floor_{};
    [[nodiscard]] constexpr bool owns(Identity id) const noexcept
    { return id.valid() && id == identity_; }
    [[nodiscard]] static constexpr bool frame(const UserFrame &f, Capability capability) noexcept
    { return capability.admitted() && arch::execution::mips_o32_private::frame_mode(f); }
  public:
    [[nodiscard]] constexpr Identity identity() const noexcept { return identity_; }
    [[nodiscard]] constexpr State state() const noexcept { return state_; }
    // Explicit creation/reuse/exec hook. No frame-shape inference occurs here.
    constexpr void reset(Identity id, Capability capability) noexcept
    { identity_ = id.valid() ? id : Identity{}; state_ = State::unverified; sequence_floor_ = capability.sequence(); }
    constexpr void revoke(Identity id) noexcept
    { if (owns(id)) state_ = State::unverified; }
    [[nodiscard]] constexpr bool ready(Identity id) const noexcept
    {
        return owns(id) && (state_ == State::initial || state_ == State::syscall_normalized ||
            state_ == State::timer_non_bd || state_ == State::handler || state_ == State::sigreturn);
    }
    // Call only after fresh initial_frame construction and ELF/mapping/stack
    // admission. It is a trusted publication event, not a lazy readiness query.
    [[nodiscard]] constexpr bool publish_initial(Identity id, const UserFrame &f, Capability capability) noexcept
    {
        if (!owns(id) || state_ != State::unverified || !frame(f, capability)) return false;
        state_ = State::initial; return true;
    }
    [[nodiscard]] constexpr bool begin_entry(Identity id, Capability capability) noexcept
    {
        if (!owns(id)) return false;
        state_ = State::awaiting_capture;
        sequence_floor_ = capability.sequence();
        return capability.admitted();
    }
    // A fresh backend observation is consumed only while awaiting this lease's
    // capture. The caller has already validated the owned privileged stack.
    [[nodiscard]] constexpr bool capture(Identity id, const UserFrame &f, Observation observation) noexcept
    {
        if (!owns(id)) return false;
        if (state_ != State::awaiting_capture || !observation.matches(f) ||
            observation.sequence() <= sequence_floor_)
        { state_ = State::unverified; return false; }
        sequence_floor_ = observation.sequence();
        using K = arch::execution::mips_o32_private::CaptureKind;
        switch (observation.kind())
        {
        case K::syscall_normalized: state_ = State::syscall_normalized; return true;
        case K::timer_non_bd: state_ = State::timer_non_bd; return true;
        case K::timer_bd: state_ = State::timer_bd; return true;
        default: state_ = State::unverified; return false;
        }
    }
    [[nodiscard]] constexpr bool inherit_syscall(Identity id, const MipsO32SignalReadiness &parent,
                                                Identity parent_id, const UserFrame &child,
                                                Capability capability) noexcept
    {
        if (!owns(id) || state_ != State::unverified || !parent.owns(parent_id) ||
            parent.state_ != State::syscall_normalized || !frame(child, capability)) return false;
        state_ = State::syscall_normalized; return true;
    }
    // These hooks run only after their corresponding fallible transaction has
    // committed. Failed preparations must never call a publication hook.
    [[nodiscard]] constexpr bool publish_handler(Identity id, const UserFrame &f, Capability capability) noexcept
    {
        if (!ready(id) || !frame(f, capability)) return false;
        state_ = State::handler; return true;
    }
    [[nodiscard]] constexpr bool publish_sigreturn(Identity id, const UserFrame &f, Capability capability) noexcept
    {
        if (!owns(id) || state_ != State::syscall_normalized || !frame(f, capability)) return false;
        state_ = State::sigreturn; return true;
    }
    [[nodiscard]] constexpr MipsO32SignalContext context(Identity id, Capability capability) const noexcept
    {
        MipsO32SignalContext result{};
        if (capability.admitted()) result.fp = MipsO32SignalContext::FpContract::eager_fr0_legacy_nan_no_extensions;
        if (ready(id)) result.resume = MipsO32SignalContext::ResumeContract::normalized_resume_pc;
        return result;
    }
};
} // namespace ok::user::native
