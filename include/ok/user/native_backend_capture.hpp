#pragma once

#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/sched/scheduler.hpp"

namespace ok::user::native
{
// One real backend entry per active lease epoch. Both observers consume this
// private active-view capability before the runtime deactivates the view.
class BackendCapture final : private NonCopyable
{
  public:
    using Pool = memory::KernelEntryStackPool;
    using Lease = memory::KernelEntryStackHandle;
    using Frame = arch::execution::UserFrame;
    using Space = arch::execution::AddressSpace;
    using Trap = arch::execution::Trap;
    using View = arch::execution::KernelEntryStackView;
    enum class Observer : u8 { restart_origin = 1, vfs_continuation = 2 };
    class Observation final
    {
      public:
        Observation(const Observation &) = delete;
        Observation &operator=(const Observation &) = delete;
        Observation &operator=(Observation &&) = delete;
        Observation(Observation &&other) noexcept
            : authority_(std::exchange(other.authority_, nullptr)), lease_(other.lease_),
              sequence_(std::exchange(other.sequence_, 0)), epoch_(other.epoch_), pid_(other.pid_), tid_(other.tid_),
              space_(other.space_), frame_(other.frame_), trap_(other.trap_) {}
        [[nodiscard]] Lease lease() const { return lease_; }
        [[nodiscard]] u64 epoch() const { return epoch_; }
        [[nodiscard]] u64 sequence() const { return sequence_; }
        [[nodiscard]] Space space() const { return space_; }
        [[nodiscard]] const Frame &frame() const { return frame_; }
        [[nodiscard]] const Trap &trap() const { return trap_; }
      private:
        friend class BackendCapture;
        Observation(BackendCapture &owner, Lease lease, u64 sequence, u64 epoch, u64 pid, u64 tid,
                    Space space, const Frame &frame, Trap trap)
            : authority_(&owner), lease_(lease), sequence_(sequence), epoch_(epoch), pid_(pid), tid_(tid),
              space_(space), frame_(frame), trap_(trap) {}
        BackendCapture *authority_{};
        Lease lease_{};
        u64 sequence_{}, epoch_{}, pid_{}, tid_{};
        Space space_{};
        Frame frame_{};
        Trap trap_{};
    };

    explicit constexpr BackendCapture(Pool &pool) : pool_(&pool) {}
    Result<Observation> enter(sched::SchedulerState &scheduler, sched::ProcessControlBlock &process,
                              sched::ThreadControlBlock &thread, const View &view)
    {
        if (!canonical(scheduler, process, thread) || thread.syscall_pending || !thread.native_active ||
            thread.state != sched::ProcessState::running || thread.entry_stack.pool != pool_)
            return Status::denied("shared backend capture has no current canonical native owner");
        const auto epoch = pool_->active_epoch(thread.entry_stack, {thread.owner, thread.tid}, view);
        if (!epoch) return epoch.status();
        const Lease lease = thread.entry_stack;
        auto &entry = entries_[lease.slot];
        if (entry.lease == lease && entry.epoch == epoch.value())
            return Status::already_exists("backend already invoked for this active lease epoch");
        if (sequence_ == ~u64{0}) return Status::overflow("shared backend capture sequence exhausted");
        const Space space = process.resources().native_space;
        // Consume the epoch even on backend failure; replay requires a new entry.
        entry = {lease, epoch.value(), ++sequence_, 0};
        auto trap = arch::execution::enter_owned(process.resources().native_space, thread.native_frame, view);
        const auto after = pool_->active_epoch(lease, {thread.owner, thread.tid}, view);
        if (!after || after.value() != epoch.value() || !canonical(scheduler, process, thread) ||
            thread.entry_stack != lease || !same_space(space, process.resources().native_space))
            return Status::denied("shared backend capture changed canonical owner or lease");
        if (!trap) return trap.status();
        const auto stack = trap.value().kernel_stack_pointer;
        const bool required = trap.value().kind == arch::execution::TrapKind::syscall ||
                              trap.value().kind == arch::execution::TrapKind::timer;
        if ((stack && !view.contains(stack)) || (required && !stack))
            return Status::denied("shared backend capture lacks its privileged stack");
        return Observation(*this, lease, entry.sequence, entry.epoch, thread.owner, thread.tid,
                           space, thread.native_frame, trap.value());
    }

    [[nodiscard]] static bool claim(const Observation &observation, Observer observer,
                                    sched::SchedulerState &scheduler, sched::ProcessControlBlock &process,
                                    sched::ThreadControlBlock &thread, const View &view)
    {
        auto *owner = observation.authority_;
        if (!owner || observation.lease_.pool != owner->pool_ || observation.lease_.slot >= Pool::capacity ||
            !observation.sequence_ || !canonical(scheduler, process, thread) || !thread.native_active ||
            thread.syscall_pending || thread.state != sched::ProcessState::running ||
            thread.entry_stack != observation.lease_ || thread.owner != observation.pid_ || thread.tid != observation.tid_ ||
            !same_space(process.resources().native_space, observation.space_) || !same_frame(thread.native_frame, observation.frame_))
            return false;
        const auto epoch = owner->pool_->active_epoch(observation.lease_, {observation.pid_, observation.tid_}, view);
        auto &entry = owner->entries_[observation.lease_.slot];
        const u8 bit = static_cast<u8>(observer);
        if ((bit != 1 && bit != 2) || !epoch || epoch.value() != observation.epoch_ || entry.lease != observation.lease_ ||
            entry.epoch != observation.epoch_ || entry.sequence != observation.sequence_ || (entry.claimed & bit)) return false;
        entry.claimed |= bit;
        return true;
    }

    [[nodiscard]] static bool same_frame(const Frame &a, const Frame &b)
    {
        return a.registers == b.registers && a.pc == b.pc && a.sp == b.sp && a.flags == b.flags &&
            a.fs_base == b.fs_base && a.gs_base == b.gs_base && a.fp_state == b.fp_state;
    }
    [[nodiscard]] static bool same_space(Space a, Space b)
    { return a.root == b.root && a.slot == b.slot && a.generation == b.generation; }
  private:
    static bool canonical(sched::SchedulerState &scheduler, sched::ProcessControlBlock &process,
                          sched::ThreadControlBlock &thread)
    {
        return process.pid() == thread.owner && scheduler.find(process.pid()) == &process &&
            scheduler.find_thread(thread.owner, thread.tid) == &thread && scheduler.current_thread() == &thread &&
            scheduler.current_pid() == process.pid();
    }
    struct Entry { Lease lease{}; u64 epoch{}, sequence{}; u8 claimed{}; };
    Pool *pool_{};
    std::array<Entry, Pool::capacity> entries_{};
    u64 sequence_{};
};
} // namespace ok::user::native
