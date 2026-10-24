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
    auto top = pool.top(thread.entry_stack, {thread.owner, thread.tid});
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

// Replacement consumes no old lease until scheduler validation has committed.
// The local escrow is the sole reference holder during the serialized commit;
// a failed commit restores every reference, including retired leader slots.
template <typename Commit, typename BeforeRetire>
EntryStackReplacement replace_entry_stacks(memory::KernelEntryStackPool &pool, sched::ProcessControlBlock &process,
                                          Commit &&commit, BeforeRetire &&before_retire)
{
    if (process.threads().empty()) return {Status::invalid_argument("exec has no leader slot")};
    std::array<RetainedEntryStack, sched::max_threads_per_process> saved{};
    const usize count = process.threads().size();
    for (usize i = 0; i < count; ++i)
    {
        const auto &thread = process.threads()[i];
        if (auto status = validate_entry_stack(pool, thread); !status) return {status};
        saved[i] = {{thread.owner, thread.tid}, thread.entry_stack, thread.kernel_stack_top};
    }
    auto leader = saved[0];
    const bool new_leader = !leader.handle.pool;
    if (new_leader)
    {
        auto reserved = pool.reserve(leader.owner);
        if (!reserved) return {reserved.status()};
        leader.handle = reserved.value();
        auto top = pool.top(leader.handle, leader.owner);
        if (!top)
        {
            // Preserve the charged lease for diagnosis if backing is damaged.
            process.threads()[0].entry_stack = leader.handle;
            return {top.status()};
        }
        leader.top = top.value();
    }
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
        if (new_leader)
            if (auto released = pool.release(leader.handle, leader.owner); !released)
            {
                process.threads()[0].entry_stack = leader.handle;
                process.threads()[0].kernel_stack_top = leader.top;
                return {released};
            }
        return {status};
    }
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

template <typename Commit>
EntryStackReplacement replace_entry_stacks(memory::KernelEntryStackPool &pool, sched::ProcessControlBlock &process, Commit &&commit)
{
    return replace_entry_stacks(pool, process, std::forward<Commit>(commit), [] { return Status{}; });
}

} // namespace ok::user::native
