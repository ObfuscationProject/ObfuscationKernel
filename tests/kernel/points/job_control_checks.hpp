#pragma once

#include "ok/sched/scheduler.hpp"

namespace ok::tests
{
// Exercise the scheduler stop overlay and the orphaned-group rule on canonical
// scheduler state during Debug boot. This is a model check: nothing sends a
// stop signal and no user thread is entered or stopped.
inline Status check_job_control_model(sched::SchedulerState &scheduler)
{
    const auto context = arch::configured_arch_operations().make_user_context({0x400000, 0x800000});
    std::array<sched::ProcessId, 3> owned{};
    const auto baseline = scheduler.stopped_processes();
    const auto result = [&]() -> Status {
        const auto leader = scheduler.create_user_process("job-leader-model", context);
        if (!leader) return leader.status();
        owned[0] = leader.value();
        if (auto status = scheduler.set_runnable(leader.value()); !status) return status;
        const auto job = scheduler.fork_process(leader.value());
        if (!job) return job.status();
        owned[1] = job.value();
        if (auto status = scheduler.set_process_group(job.value(), 0, 0); !status) return status;
        const auto peer = scheduler.fork_process(job.value());
        if (!peer) return peer.status();
        owned[2] = peer.value();
        // The leader's session parent keeps the job group attached; without
        // the job leader only the peer, whose parent is in the group, remains.
        if (scheduler.process_group_orphaned(job.value()) || !scheduler.process_group_orphaned(job.value(), job.value()))
            return Status::fault("job control orphaned-group model failed");
        const sched::JobControlState clean{};
        if (scheduler.find(job.value())->job_control() != clean ||
            scheduler.stop_process(job.value(), 0).code() != StatusCode::invalid_argument ||
            scheduler.continue_process(job.value()))
            return Status::fault("job control initial overlay model failed");
        if (auto status = scheduler.stop_process(job.value(), 19); !status) return status;
        const auto *process = scheduler.find(job.value());
        if (!process || process->job_control() != sched::JobControlState{19, true, true, false, 0} ||
            scheduler.stopped_processes() != baseline + 1 || !scheduler.process_group_has_stopped(job.value()) ||
            scheduler.activate_thread(job.value(), job.value()).ok() ||
            process->state() == sched::ProcessState::zombie)
            return Status::fault("job control stop overlay model failed");
        if (!scheduler.continue_process(job.value()) || scheduler.continue_process(job.value()))
            return Status::fault("job control continue model failed");
        process = scheduler.find(job.value());
        if (!process || process->job_control() != sched::JobControlState{19, false, false, true, 1} ||
            scheduler.stopped_processes() != baseline || scheduler.process_group_has_stopped(job.value()))
            return Status::fault("job control continue overlay model failed");
        scheduler.consume_job_report(job.value(), false);
        // Exit clears a stopped process's overlay and its count.
        if (auto status = scheduler.stop_process(peer.value(), 20); !status) return status;
        if (auto status = scheduler.exit_process(peer.value(), 0); !status) return status;
        process = scheduler.find(peer.value());
        if (!process || process->job_control() != clean || scheduler.stopped_processes() != baseline ||
            scheduler.stop_process(peer.value(), 19).code() != StatusCode::already_exists)
            return Status::fault("job control exit overlay model failed");
        return Status::success();
    }();
    // Retire by scalar PID: reaping compacts canonical process storage.
    for (usize index = owned.size(); index > 0; --index)
    {
        const auto pid = owned[index - 1];
        const auto *process = pid ? scheduler.find(pid) : nullptr;
        if (!process) continue;
        if (process->state() != sched::ProcessState::zombie && process->state() != sched::ProcessState::exited)
            if (auto status = scheduler.exit_process(pid, 0); !status) return status;
        if (auto status = scheduler.reap_process(pid); !status) return status;
    }
    if (result.ok() && scheduler.stopped_processes() != baseline)
        return Status::fault("job control model leaked a stopped process");
    return result;
}
} // namespace ok::tests
