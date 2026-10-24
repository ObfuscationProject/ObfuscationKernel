#pragma once
#include "ok/user/native_signal_readiness_ppc32.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"

namespace ok::user::native
{
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
