// Included in scheduler.cpp inside ok::sched. Group transitions (spec 4.8):
// the exec and exit claims that freeze a thread group, the thaw of a failed
// exec, and the pending exit that a kill sets without waiting. The waiting
// itself is the claimer's (group_quiesce.hpp); every function here holds one
// task lock for a bounded time.
SchedulerState::GroupClaim SchedulerState::claim_transition(ProcessId pid, ThreadId claimer, GroupTransition kind,
                                                            ExitRequest request, bool quiesced_only)
{
    GroupClaim claim{};
    if (kind == GroupTransition::none) return claim;
    auto *slot = visible_slot(pid);
    if (!slot) return claim;
    TaskGuard guard{*slot};
    auto &process = slot->pcb;
    if (slot->state() != ProcessLifecycle::live || process.state_ == ProcessState::zombie ||
        process.state_ == ProcessState::exited)
        return claim;
    if (slot->transition == GroupTransition::exit)
    {
        claim.outcome = slot->transition_tid == claimer ? ClaimOutcome::held : ClaimOutcome::exit_held;
        return claim;
    }
    if (slot->transition == GroupTransition::exec)
    {
        if (slot->transition_tid == claimer)
        {
            // The exec claimer exits instead (its commit check found a
            // pending exit): the group stays frozen and quiesced.
            if (kind == GroupTransition::exit) slot->transition = GroupTransition::exit;
            claim.outcome = ClaimOutcome::held;
            return claim;
        }
        if (kind == GroupTransition::exec)
        {
            // Linux de_thread: the winner's commit exits this thread.
            claim.outcome = ClaimOutcome::exec_held;
            return claim;
        }
        // Exit beats exec: the exec claimer aborts at its commit check.
        if (!slot->flagged(RunFlag::pending_exit))
        {
            slot->exit_request = request;
            slot->set_flag(RunFlag::pending_exit, true);
        }
        claim.outcome = ClaimOutcome::exit_pending;
        return claim;
    }
    if (quiesced_only)
        for (const auto &thread : process.live_threads())
            if (thread.tid != claimer && thread.on_cpu != no_cpu)
            {
                claim.outcome = ClaimOutcome::busy;
                return claim;
            }
    slot->transition = kind;
    slot->transition_tid = static_cast<u32>(claimer);
    slot->set_flag(RunFlag::frozen, true);
    for (auto &thread : process.live_threads())
    {
        if (thread.tid == claimer) continue;
        forget_queued_locked(*slot, thread);
        if (thread.on_cpu < smp::max_cpus) claim.kick |= 1u << thread.on_cpu;
    }
    claim.outcome = ClaimOutcome::claimed;
    return claim;
}

u32 SchedulerState::group_cpus(ProcessId pid, ThreadId claimer) const
{
    const auto *slot = visible_slot(pid);
    if (!slot) return 0;
    u32 cpus = 0;
    const auto &threads = slot->pcb.threads_;
    for (usize index = 0; index < max_threads_per_process; ++index)
    {
        // Only the claimer's slot is skipped: every other placement word
        // must read no_cpu, whatever the slot's state. The load pairs with
        // put_prev's release store, its last write to the thread.
        if (claimer != 0 && threads[index].tid == claimer) continue;
        const auto cpu = slot->on_cpu[index].load(std::memory_order_acquire);
        if (cpu < smp::max_cpus) cpus |= 1u << cpu;
    }
    return cpus;
}

bool SchedulerState::group_quiesced(ProcessId pid, ThreadId claimer) const
{
    return group_cpus(pid, claimer) == 0;
}

void SchedulerState::unfreeze(ProcessId pid, ThreadId claimer)
{
    auto *slot = visible_slot(pid);
    if (!slot) return;
    u32 placed = 0;
    {
        TaskGuard guard{*slot};
        if (slot->transition != GroupTransition::exec || slot->transition_tid != claimer) return;
        slot->transition = GroupTransition::none;
        slot->transition_tid = 0;
        slot->set_flag(RunFlag::frozen, false);
        // Every runnable, probed or exiting thread off its CPU is queued
        // again; none lost user progress (its frame was kept).
        for (auto &thread : slot->pcb.live_threads())
            if (thread.on_cpu == no_cpu)
                if (const auto cpu = enqueue_locked(*slot, thread); cpu != no_cpu) placed |= 1u << cpu;
    }
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        if ((placed & (1u << cpu)) != 0) notify_cpu(cpu);
}

std::optional<ExitRequest> SchedulerState::exec_superseded(ProcessId pid, ThreadId claimer)
{
    auto *slot = visible_slot(pid);
    if (!slot) return std::nullopt;
    TaskGuard guard{*slot};
    if (slot->transition != GroupTransition::exec || slot->transition_tid != claimer ||
        !slot->flagged(RunFlag::pending_exit))
        return std::nullopt;
    slot->transition = GroupTransition::exit;
    return slot->exit_request;
}

u32 SchedulerState::request_group_exit(ProcessId pid, ExitRequest request)
{
    auto *slot = visible_slot(pid);
    if (!slot) return 0;
    u32 kick = 0;
    u32 placed = 0;
    {
        TaskGuard guard{*slot};
        auto &process = slot->pcb;
        if (slot->state() != ProcessLifecycle::live || process.state_ == ProcessState::zombie ||
            process.state_ == ProcessState::exited)
            return 0;
        // The first request wins, except that SIGKILL overrides a fatal
        // default (Linux: SIGKILL always acts).
        if (!slot->flagged(RunFlag::pending_exit) || (request.signal == 9 && slot->exit_request.signal != 9))
            slot->exit_request = request;
        slot->set_flag(RunFlag::pending_exit, true);
        // An exit claim already runs; an exec claimer finds the request at
        // its commit check. Neither needs a probe or an entry.
        if (slot->transition != GroupTransition::none) return 0;
        for (auto &thread : process.live_threads())
        {
            // A waiting thread stays blocked: its pick admits it and the
            // delivery point exits the group before anything resumes.
            if (thread.state == ProcessState::blocked) thread.probe = true;
            if (thread.on_cpu < smp::max_cpus)
                kick |= 1u << thread.on_cpu;
            else if (const auto cpu = enqueue_locked(*slot, thread); cpu != no_cpu)
                placed |= 1u << cpu;
        }
    }
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        if ((placed & (1u << cpu)) != 0) notify_cpu(cpu);
    return kick;
}

std::optional<ExitRequest> SchedulerState::pending_exit(ProcessId pid)
{
    auto *slot = visible_slot(pid);
    if (!slot || !slot->flagged(RunFlag::pending_exit)) return std::nullopt;
    TaskGuard guard{*slot};
    if (!slot->flagged(RunFlag::pending_exit)) return std::nullopt;
    return slot->exit_request;
}

bool SchedulerState::exit_pending(ProcessId pid) const
{
    const auto *slot = visible_slot(pid);
    return slot && slot->flagged(RunFlag::pending_exit);
}

bool SchedulerState::frozen(ProcessId pid) const
{
    const auto *slot = visible_slot(pid);
    return slot && slot->flagged(RunFlag::frozen);
}

GroupTransition SchedulerState::transition(ProcessId pid) const
{
    const auto *slot = visible_slot(pid);
    return slot ? slot->transition : GroupTransition::none;
}

Status SchedulerState::publish_fork(ProcessId parent, ProcessId child)
{
    {
        // A kill(-pgrp) enumerates the group under the task table and sets
        // pending_exit on the parent there: either it saw this child, or the
        // parent's flag is visible here and the child is undone.
        const TableHold table{task_table_};
        auto *slot = visible_slot(parent);
        if (!slot) return Status::not_found("fork parent departed");
        {
            TaskGuard guard{*slot};
            if (slot->flagged(RunFlag::pending_exit) || slot->transition == GroupTransition::exit)
                return Status::interrupted("fork parent has a pending exit");
        }
        // Published under the task table, after the parent's task lock.
        return set_runnable(child);
    }
}
