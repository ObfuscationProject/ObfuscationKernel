#pragma once

#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/sched/scheduler.hpp"

namespace ok::user::native
{

// These operations run on the serialized service stack, after the backend has
// detached its privileged entry pointers. TCB copies never own the backing.
inline Status validate_entry_stack(memory::KernelEntryStackPool &pool, const sched::ThreadControlBlock &thread)
{
    if (!thread.entry_stack.pool)
        return thread.kernel_stack_top == 0 ? Status::success()
            : Status::fault("entry stack top has no lease");
    // Another CPU may be running the thread on this lease.
    auto top = pool.lease_top(thread.entry_stack, {thread.owner, thread.tid});
    if (!top) return top.status();
    return top.value() == thread.kernel_stack_top ? Status::success()
        : Status::fault("entry stack top disagrees with canonical lease");
}

inline Status prepare_entry_stack(memory::KernelEntryStackPool &pool, sched::ThreadControlBlock &thread)
{
    if (thread.entry_stack.pool || thread.kernel_stack_top)
        return Status::already_exists("thread already references an entry stack");
    auto lease = pool.reserve({thread.owner, thread.tid});
    if (!lease) return lease.status();
    thread.entry_stack = lease.value();
    auto top = pool.top(thread.entry_stack, {thread.owner, thread.tid});
    if (!top) return top.status();
    thread.kernel_stack_top = top.value();
    return Status::success();
}

inline Status retire_entry_stack(memory::KernelEntryStackPool &pool, sched::ThreadControlBlock &thread)
{
    if (auto status = validate_entry_stack(pool, thread); !status) return status;
    if (!thread.entry_stack.pool) return Status::success();
    if (auto status = pool.release(thread.entry_stack, {thread.owner, thread.tid}); !status) return status;
    thread.entry_stack = {};
    thread.kernel_stack_top = 0;
    return Status::success();
}

struct RetainedEntryStack
{
    memory::KernelEntryStackOwner owner{};
    memory::KernelEntryStackHandle handle{};
    uptr top{};
};
struct EntryStackReplacement
{
    Status status{};
    bool committed{};
    // On post-commit failure, the caller must move these unreleased leases to
    // stable terminal storage before this result leaves scope.
    std::array<RetainedEntryStack, sched::max_threads_per_process> retained{};
    usize retained_count{};
    [[nodiscard]] explicit operator bool() const { return status.ok(); }
};

// The fallible half of an entry stack replacement. Exec prepares it while
// sibling threads may still run on other CPUs: every lease must be healthy
// (it may be active), and a leader without a lease receives a new one. Nothing
// the process can observe changes; an unconsumed plan releases that reservation.
class EntryStackPlan;
inline Status prepare_entry_stacks(memory::KernelEntryStackPool &pool, sched::ProcessControlBlock &process,
                                   EntryStackPlan &plan);
class EntryStackPlan final
{
  public:
    EntryStackPlan() = default;
    EntryStackPlan(const EntryStackPlan &) = delete;
    EntryStackPlan &operator=(const EntryStackPlan &) = delete;
    ~EntryStackPlan() { static_cast<void>(cancel()); }

    // Releases a reserved new leader lease. A damaged one stays quarantined in
    // its pool, whose sticky fault then refuses further reservations.
    Status cancel()
    {
        const auto status = owns_leader_ ? pool_->release(leader_.handle, leader_.owner) : Status::success();
        owns_leader_ = false;
        pool_ = nullptr;
        count_ = 0;
        return status;
    }
    [[nodiscard]] bool prepared() const { return pool_ != nullptr; }
    [[nodiscard]] const RetainedEntryStack &leader() const { return leader_; }

  private:
    template <typename Commit, typename BeforeRetire>
    friend EntryStackReplacement commit_entry_stacks(memory::KernelEntryStackPool &, sched::ProcessControlBlock &,
                                                     EntryStackPlan &, Commit &&, BeforeRetire &&);
    friend Status prepare_entry_stacks(memory::KernelEntryStackPool &, sched::ProcessControlBlock &, EntryStackPlan &);
    memory::KernelEntryStackPool *pool_{};
    std::array<RetainedEntryStack, sched::max_threads_per_process> saved_{};
    usize count_{};
    RetainedEntryStack leader_{};
    bool owns_leader_{};
};

inline Status prepare_entry_stacks(memory::KernelEntryStackPool &pool, sched::ProcessControlBlock &process,
                                   EntryStackPlan &plan)
{
    if (plan.prepared()) return Status::already_exists("entry stack plan is already prepared");
    if (process.threads().empty()) return Status::invalid_argument("exec has no leader slot");
    const usize count = process.threads().size();
    for (usize i = 0; i < count; ++i)
    {
        const auto &thread = process.threads()[i];
        if (auto status = validate_entry_stack(pool, thread); !status) return status;
        plan.saved_[i] = {{thread.owner, thread.tid}, thread.entry_stack, thread.kernel_stack_top};
    }
    plan.leader_ = plan.saved_[0];
    if (!plan.leader_.handle.pool)
    {
        auto reserved = pool.reserve(plan.leader_.owner);
        if (!reserved) return reserved.status();
        plan.leader_.handle = reserved.value();
        auto top = pool.top(plan.leader_.handle, plan.leader_.owner);
        if (!top)
        {
            // Preserve the charged lease for diagnosis if backing is damaged.
            process.threads()[0].entry_stack = plan.leader_.handle;
            return top.status();
        }
        plan.leader_.top = top.value();
        plan.owns_leader_ = true;
    }
    plan.pool_ = &pool;
    plan.count_ = count;
    return Status::success();
}

// The committing half: replacement consumes no old lease until scheduler
// validation has committed. The local escrow is the sole reference holder
// during the serialized commit; a failed commit restores every reference,
// including retired leader slots. Every old lease must be inactive by now
// (exec evicted the siblings), so a refusal here is an invariant breach.
template <typename Commit, typename BeforeRetire>
EntryStackReplacement commit_entry_stacks(memory::KernelEntryStackPool &pool, sched::ProcessControlBlock &process,
                                          EntryStackPlan &plan, Commit &&commit, BeforeRetire &&before_retire)
{
    if (plan.pool_ != &pool || plan.count_ != process.threads().size())
        return {Status::fault("entry stack plan does not match the thread group")};
    const usize count = plan.count_;
    const auto &saved = plan.saved_;
    for (usize i = 0; i < count; ++i)
    {
        const auto &thread = process.threads()[i];
        if (thread.owner != saved[i].owner.pid || thread.tid != saved[i].owner.tid ||
            thread.entry_stack != saved[i].handle || thread.kernel_stack_top != saved[i].top)
            return {Status::fault("entry stack plan does not match the thread group")};
        // Replacement releases these leases: none may still be in use.
        if (thread.entry_stack.pool)
            if (auto status = pool.validate_inactive(thread.entry_stack, {thread.owner, thread.tid}); !status)
                return {status};
    }
    const auto leader = plan.leader_;
    for (auto &thread : process.threads())
    {
        thread.entry_stack = {};
        thread.kernel_stack_top = 0;
    }
    const auto status = commit();
    if (!status)
    {
        for (usize i = 0; i < count; ++i)
        {
            process.threads()[i].entry_stack = saved[i].handle;
            process.threads()[i].kernel_stack_top = saved[i].top;
        }
        if (auto released = plan.cancel(); !released)
        {
            process.threads()[0].entry_stack = leader.handle;
            process.threads()[0].kernel_stack_top = leader.top;
            return {released};
        }
        return {status};
    }
    // The leader's TCB owns the plan's lease from here on.
    plan.owns_leader_ = false;
    static_cast<void>(plan.cancel());
    auto &thread = process.threads()[0];
    thread.entry_stack = leader.handle;
    thread.kernel_stack_top = leader.top;
    // Scheduler replacement is committed even if later retirement finds damage.
    EntryStackReplacement result{Status::success(), true};
    if (auto cleanup = before_retire(); !cleanup)
    {
        result.status = cleanup;
        for (usize i = 1; i < count; ++i)
            if (saved[i].handle.pool) result.retained[result.retained_count++] = saved[i];
        return result;
    }
    for (usize i = 1; i < count; ++i)
        if (saved[i].handle.pool)
        {
            if (result.status)
                result.status = pool.release(saved[i].handle, saved[i].owner);
            if (!result.status)
                result.retained[result.retained_count++] = saved[i];
        }
    return result;
}

// Both halves at once, for callers whose threads are all inactive already.
template <typename Commit, typename BeforeRetire>
EntryStackReplacement replace_entry_stacks(memory::KernelEntryStackPool &pool, sched::ProcessControlBlock &process,
                                          Commit &&commit, BeforeRetire &&before_retire)
{
    if (process.threads().empty()) return {Status::invalid_argument("exec has no leader slot")};
    for (const auto &thread : process.threads())
    {
        if (auto status = validate_entry_stack(pool, thread); !status) return {status};
        if (thread.entry_stack.pool)
            if (auto status = pool.validate_inactive(thread.entry_stack, {thread.owner, thread.tid}); !status)
                return {status};
    }
    EntryStackPlan plan;
    if (auto status = prepare_entry_stacks(pool, process, plan); !status) return {status};
    return commit_entry_stacks(pool, process, plan, std::forward<Commit>(commit),
                               std::forward<BeforeRetire>(before_retire));
}

template <typename Commit>
EntryStackReplacement replace_entry_stacks(memory::KernelEntryStackPool &pool, sched::ProcessControlBlock &process, Commit &&commit)
{
    return replace_entry_stacks(pool, process, std::forward<Commit>(commit), [] { return Status{}; });
}

} // namespace ok::user::native
