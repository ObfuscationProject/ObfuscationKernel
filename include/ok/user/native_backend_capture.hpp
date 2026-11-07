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
        [[nodiscard]] u64 pid() const { return pid_; }
        [[nodiscard]] u64 tid() const { return tid_; }
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

    // The backend entry. The native runtime installs one that leaves the
    // kernel lock while user code runs on this CPU; the thread is named by its
    // identity because its records may move while the lock is not held.
    // Without one, the backend is entered directly.
    using Entry = Result<Trap> (*)(Space &space, Frame &frame, const View &view, Lease lease, u64 pid, u64 tid);
    explicit constexpr BackendCapture(Pool &pool, Entry entry = nullptr) : pool_(&pool), entry_(entry) {}
    void set_entry(Entry entry) { entry_ = entry; }
    // On success `process`/`thread` name the canonical records again, which
    // may have moved. A no_process status means another CPU retired the
    // thread while it ran: its lease and frame are no longer this caller's.
    Result<Observation> enter(sched::SchedulerState &scheduler, sched::ProcessControlBlock *&process,
                              sched::ThreadControlBlock *&thread, const View &view)
    {
        if (!process || !thread || !canonical(scheduler, *process, *thread) || thread->syscall_pending ||
            !thread->native_active || thread->state != sched::ProcessState::running || thread->entry_stack.pool != pool_)
            return Status::denied("shared backend capture has no current canonical native owner");
        const u64 pid = thread->owner, tid = thread->tid;
        const auto epoch = pool_->active_epoch(thread->entry_stack, {pid, tid}, view);
        if (!epoch) return epoch.status();
        const Lease lease = thread->entry_stack;
        auto &entry = entries_[lease.slot];
        if (entry.lease == lease && entry.epoch == epoch.value())
            return Status::already_exists("backend already invoked for this active lease epoch");
        if (sequence_ == ~u64{0}) return Status::overflow("shared backend capture sequence exhausted");
        Space space = process->resources().native_space;
        // Consume the epoch even on backend failure; replay requires a new entry.
        entry = {lease, epoch.value(), ++sequence_, 0};
        Frame frame = thread->native_frame;
        auto trap = entry_ ? entry_(space, frame, view, lease, pid, tid)
                           : arch::execution::enter_owned(space, frame, view);
        if (!trap && trap.status().code() == StatusCode::no_process) return trap.status();
        process = scheduler.find(pid);
        thread = process ? scheduler.find_thread(pid, tid) : nullptr;
        if (!process || !thread) return Status::no_process("native thread retired during its user entry");
        thread->native_frame = frame;
        // The backend ran on the copy; its capture now names the canonical frame.
        arch::execution::rebind_owned_capture(frame, thread->native_frame);
        const auto after = pool_->active_epoch(lease, {pid, tid}, view);
        if (!after || after.value() != epoch.value() || !canonical(scheduler, *process, *thread) ||
            thread->entry_stack != lease)
            return Status::denied("shared backend capture changed canonical owner or lease");
        // A sibling on another CPU may have replaced the address space while
        // this thread ran (mmap, munmap, mprotect): the old tables were
        // quiesced before they were freed, and the trap belongs to the live one.
        space = process->resources().native_space;
        if (!trap) return trap.status();
        const auto stack = trap.value().kernel_stack_pointer;
        const bool required = trap.value().kind == arch::execution::TrapKind::syscall ||
                              trap.value().kind == arch::execution::TrapKind::timer ||
                              trap.value().kind == arch::execution::TrapKind::interrupt;
        if ((stack && !view.contains(stack)) || (required && !stack))
            return Status::denied("shared backend capture lacks its privileged stack");
        return Observation(*this, lease, entry.sequence, entry.epoch, pid, tid,
                           space, thread->native_frame, trap.value());
    }
    Result<Observation> enter(sched::SchedulerState &scheduler, sched::ProcessControlBlock &process,
                              sched::ThreadControlBlock &thread, const View &view)
    {
        auto *owner = &process;
        auto *current = &thread;
        return enter(scheduler, owner, current, view);
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
    struct Record { Lease lease{}; u64 epoch{}, sequence{}; u8 claimed{}; };
    Pool *pool_{};
    Entry entry_{};
    std::array<Record, Pool::capacity> entries_{};
    u64 sequence_{};
};
} // namespace ok::user::native
