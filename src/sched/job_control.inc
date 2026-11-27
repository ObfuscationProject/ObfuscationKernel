// Included in scheduler.cpp inside ok::sched. Job-control overlay (spec 4.9).
// Stopping or continuing never touches thread states or wait registrations:
// blocked waits stay armed and still complete while the process is stopped.
// The stop flag is mirrored in the slot's run flags under its task lock.
// Signal generation calls both under the task table (spec 4.7); each takes
// the process's task lock and wakes waiters after releasing it.
void SchedulerState::wake_job_waiters(const ProcessControlBlock &process)
{
    wake_waiters(ThreadWaitReason::child, process.pid());
    wake_waiters(ThreadWaitReason::child, 0);
    wake_waiters(ThreadWaitReason::child, child_group_wait_key(process.process_group_id()));
}
Status SchedulerState::stop_process(ProcessId pid, u8 signal)
{
    auto *process = find(pid);
    if (!process) return Status::not_found("process not found");
    if (process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::already_exists("process has already exited");
    if (!signal) return Status::invalid_argument("stop requires a signal");
    if (process->job_.stopped) return Status::success();
    u32 kick = 0;
    {
        // Queued entries are dropped when popped; running threads trap out
        // (Linux kick_process), not at their next tick.
        auto &slot = task_slot(*process);
        TaskGuard guard{slot};
        process->job_.stopped = true;
        process->job_.stop_signal = signal;
        process->job_.stop_report = true;
        process->job_.continue_report = false;
        slot.set_flag(RunFlag::stopped, true);
        for (const auto &thread : process->live_threads())
            if (thread.on_cpu < smp::max_cpus) kick |= 1u << thread.on_cpu;
    }
    const auto self = smp::current_cpu();
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        if (cpu != self && (kick & (1u << cpu)) != 0) smp::send_ipi(cpu, smp::ipi_kick);
    wake_job_waiters(*process);
    return Status::success();
}
bool SchedulerState::continue_process(ProcessId pid)
{
    auto *process = find(pid);
    if (!process || !process->job_.stopped) return false;
    u32 placed = 0;
    {
        // Every off-CPU thread that is runnable, probed or exiting is queued
        // again (spec 4.9): a probe set while stopped is honored now.
        auto &slot = task_slot(*process);
        TaskGuard guard{slot};
        process->job_.stopped = false;
        process->job_.stop_report = false;
        process->job_.continue_report = true;
        ++process->job_.continue_generation;
        slot.set_flag(RunFlag::stopped, false);
        for (auto &thread : process->live_threads())
            if (thread.on_cpu == no_cpu)
                if (const auto cpu = enqueue_locked(slot, thread); cpu != no_cpu) placed |= 1u << cpu;
    }
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
        if ((placed & (1u << cpu)) != 0) notify_cpu(cpu);
    wake_job_waiters(*process);
    // Terminal calls blocked across the stop re-run their job-control check
    // (Linux restarts them after the stop interrupted the wait).
    for (auto &thread : process->thread_records())
        if (thread.state == ProcessState::blocked &&
            (thread.wait_reason == ThreadWaitReason::console_read || thread.wait_reason == ThreadWaitReason::pty_read ||
             thread.wait_reason == ThreadWaitReason::pty_write || thread.wait_reason == ThreadWaitReason::job_control))
            static_cast<void>(complete_wait(thread.wait_registration.ticket(), WaitCompletion::ready));
    return true;
}
void SchedulerState::consume_job_report(ProcessId pid, bool stop)
{
    if (auto *process = find(pid)) (stop ? process->job_.stop_report : process->job_.continue_report) = false;
}
bool SchedulerState::process_group_orphaned(ProcessId group, ProcessId ignored) const
{
    return job_group_orphaned(processes(), group, ignored, user_init_pid, [](const ProcessControlBlock &process) {
        return JobMember{process.pid(), process.parent_pid(), process.process_group_id(), process.session_id(),
                         process.state() != ProcessState::zombie && process.state() != ProcessState::exited};
    });
}
u16 SchedulerState::stopped_processes() const
{
    u16 stopped = 0;
    for (const auto &slot : slots_) stopped += slot.visible() && slot.flagged(RunFlag::stopped);
    return stopped;
}
bool SchedulerState::process_group_has_stopped(ProcessId group) const
{
    for (const auto &process : process_range())
        if (process.process_group_id() == group && process.job_.stopped) return true;
    return false;
}
