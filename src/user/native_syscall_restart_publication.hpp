#pragma once

#include "native_syscall_origin.hpp"
#include "ok/user/native_signal_persistent_ppc32.hpp"
#include "ok/user/native_signal_readiness_mips64_n64.hpp"

namespace ok::user::native
{
// Internal coordination only: callers cannot manufacture a transaction from a
// Trap or a Plan. The origin table must still own the exact wait at commit.
template <abi::linux_user::Profile Selected>
class SyscallRestartPublication final
{
    static_assert(Selected == abi::linux_user::Profile::mips64 ||
                  Selected == abi::linux_user::Profile::ppc);
    static constexpr bool n64 = Selected == abi::linux_user::Profile::mips64;
  public:
    using Origin = SyscallOriginTable<Selected>;
    using Frame = arch::execution::UserFrame;
    using Adapter = RestartAdapter<Selected>;
    using Readiness = std::conditional_t<n64, Mips64N64SignalReadiness, Ppc32PersistentReadiness>;
    using Identity = typename Readiness::Identity;
    using Capability = typename Readiness::Capability;
    using OriginIdentity = typename Origin::Identity;
    using Binding = typename Origin::PipeBinding;
    using Permit = typename Origin::RestartPermit;

    template <typename WaitBinding> requires Origin::template supported_binding<WaitBinding>
    class BasicTransaction final
    {
        friend class SyscallRestartPublication;
        Origin *origin_{};
        Readiness *readiness_{};
        const Frame *canonical_{};
        Identity identity_{};
        Capability capability_{};
        typename Origin::template BoundRestartPermit<WaitBinding> permit_{};
        u64 sequence_{};
        bool active_{};
      public:
        BasicTransaction() = default;
        [[nodiscard]] const Frame &candidate() const noexcept { return permit_.plan().frame; }
    };
    using Transaction = BasicTransaction<Binding>;
    using FutexTransaction = BasicTransaction<typename Origin::FutexBinding>;
    using ChildTransaction = BasicTransaction<typename Origin::ChildBinding>;
  private:
    [[nodiscard]] static bool same(const Frame &a, const Frame &b) noexcept
    {
        return a.registers == b.registers && a.pc == b.pc && a.sp == b.sp &&
            a.flags == b.flags && a.fs_base == b.fs_base && a.gs_base == b.gs_base &&
            a.fp_state == b.fp_state;
    }
    [[nodiscard]] static bool matched(Readiness &state, Identity id, const OriginIdentity &origin) noexcept
    {
        // The two image sequences have different issuers. Validate each against
        // its own table, and join them through the exact retained lease/space.
        if (id.pid != origin.pid || id.tid != origin.tid || !id.image_generation || !origin.image ||
            id.space.root != origin.space.root || id.space.slot != origin.space.slot ||
            id.space.generation != origin.space.generation || !origin.lease.pool) return false;
        if constexpr (n64)
            return id.pool == reinterpret_cast<uptr>(origin.lease.pool) && id.slot == origin.lease.slot &&
                id.lease_generation == origin.lease.generation && id.lower == origin.lower && id.top == origin.top;
        else
            return state.lease_.pool_ == origin.lease.pool && state.lease_.handle_ == origin.lease &&
                state.lease_.owner_ == memory::KernelEntryStackOwner{origin.pid, origin.tid} &&
                state.lease_.retained(origin.epoch);
    }
    [[nodiscard]] static bool pending(Readiness &state, Identity id, const Frame &frame,
                                      Capability cap) noexcept
    {
        if constexpr (n64)
            return !state.relocation_pending_ && state.owns(id) && state.capable(cap) &&
                state.state_ == Readiness::State::syscall_normalized && state.actual_syscall_ &&
                state.unchanged(frame);
        else
            return state.linux() && state.pending(id, frame, cap) &&
                state.diagnostics_.event == arch::execution::ppc32_private::Event::syscall &&
                state.diagnostics_.validity == arch::execution::ppc32_private::original_r3_valid;
    }
    template <typename WaitBinding>
    [[nodiscard]] static bool staged(const BasicTransaction<WaitBinding> &transaction, Identity id,
                                     const Frame &frame, Capability cap) noexcept
    {
        auto &state = *transaction.readiness_;
        if (!transaction.active_ || transaction.canonical_ != &frame || !(transaction.identity_ == id) ||
            state.sequence_floor_ != transaction.sequence_ ||
            !cap.same_issuer(transaction.capability_) || !same(frame, transaction.permit_.capture()) ||
            !same(state.saved_, frame)) return false;
        if constexpr (n64)
            return state.owns(id) && state.capable(cap) && state.frame_ == &frame &&
                state.state_ == Readiness::State::restart_staged && !state.relocation_pending_;
        else
            return state.owns(id, cap) && state.frame_ == &frame &&
                state.lease_.retained(state.lease_epoch_) &&
                state.state_ == Readiness::State::syscall_pending &&
                state.transaction_ == Readiness::Transaction::restart &&
                same(state.candidate_, transaction.candidate());
    }
    template <typename WaitBinding>
    [[nodiscard]] static bool owns_stage(const BasicTransaction<WaitBinding> &transaction) noexcept
    {
        if (!transaction.active_ || !transaction.readiness_) return false;
        const auto &state = *transaction.readiness_;
        if (!(state.identity_ == transaction.identity_) || state.frame_ != transaction.canonical_ ||
            state.sequence_floor_ != transaction.sequence_ ||
            !state.capability_.same_issuer(transaction.capability_)) return false;
        if constexpr (n64) return state.state_ == Readiness::State::restart_staged;
        else return state.state_ == Readiness::State::syscall_pending &&
            state.transaction_ == Readiness::Transaction::restart;
    }
    static void revoke(Readiness &state) noexcept
    {
        if constexpr (n64) state.revoke(state.identity());
        else state.revoke();
    }
  public:
    template <typename WaitBinding> requires Origin::template supported_binding<WaitBinding>
    [[nodiscard]] static Result<BasicTransaction<WaitBinding>> stage(Origin &origin, const OriginIdentity &owner,
        const WaitBinding &binding, Readiness &state, Identity id, const Frame &frame, Capability cap)
    {
        if (!matched(state, id, owner) || !pending(state, id, frame, cap))
            return Status::fault("restart publication lacks pending owner");
        auto permit = origin.prepare_restart(owner, frame, binding);
        if (!permit || !origin.validate_restart(permit.value(), owner, frame, binding) ||
            !same(state.saved_, permit.value().capture()))
            return Status::fault("restart publication lacks owned capture");
        const auto &plan = permit.value().plan();
        if (plan.site.bytes > plan.opcode.size())
            return Status::fault("restart publication has invalid instruction span");
        auto expected = Adapter::prepare(state.saved_, permit.value().trap(),
            std::span<const u8>{plan.opcode}.first(plan.site.bytes),
            arch::execution::user_begin, arch::execution::user_end);
        if (!expected || !same(expected.value().frame, plan.frame) ||
            expected.value().site.instruction != plan.site.instruction ||
            expected.value().site.bytes != plan.site.bytes || expected.value().site.thumb != plan.site.thumb ||
            expected.value().opcode != plan.opcode || expected.value().ppc_result != plan.ppc_result)
            return Status::fault("restart publication candidate is not exact");
        BasicTransaction<WaitBinding> out;
        out.origin_ = &origin; out.readiness_ = &state; out.canonical_ = &frame;
        out.identity_ = id; out.capability_ = cap; out.permit_ = permit.value();
        out.sequence_ = state.sequence_floor_; out.active_ = true;
        if constexpr (n64) state.state_ = Readiness::State::restart_staged;
        else
        {
            state.candidate_ = plan.frame;
            state.transaction_ = Readiness::Transaction::restart;
        }
        return out;
    }

    // Commit before retiring the old owned wait. No scheduler/user boundary may
    // intervene before the caller retires it and publishes the selected handler.
    template <typename WaitBinding> requires Origin::template supported_binding<WaitBinding>
    [[nodiscard]] static bool commit(BasicTransaction<WaitBinding> &transaction, const OriginIdentity &owner,
        const WaitBinding &binding, Identity id, Frame &frame, Capability cap) noexcept
    {
        if (!transaction.active_ || !transaction.origin_ || !transaction.readiness_) return false;
        auto &state = *transaction.readiness_;
        const bool valid = matched(state, id, owner) && staged(transaction, id, frame, cap) &&
            transaction.origin_->validate_restart(transaction.permit_, owner, frame, binding);
        if (!valid || !transaction.origin_->consume_restart(transaction.permit_, owner, frame, binding))
        {
            if (owns_stage(transaction)) revoke(state);
            transaction.active_ = false; return false;
        }
        frame = transaction.candidate();
        if constexpr (n64) state.remember(frame, Readiness::State::restart_prepared, false);
        else
        {
            state.diagnostics_.result = transaction.permit_.plan().ppc_result;
            state.diagnostics_.validity |= arch::execution::ppc32_private::result_valid;
            state.remember(frame, Readiness::State::restart_prepared);
        }
        transaction.active_ = false;
        return true;
    }

    template <typename WaitBinding>
    static void cancel(BasicTransaction<WaitBinding> &transaction) noexcept
    {
        if (owns_stage(transaction)) revoke(*transaction.readiness_);
        transaction.active_ = false;
    }
};
} // namespace ok::user::native
