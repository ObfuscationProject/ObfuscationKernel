#pragma once

#include "ok/sched/scheduler.hpp"
#include "ok/user/native_process_group_signal.hpp"

namespace ok::tests
{
// Exercise canonical scheduler state during Debug boot. This is a lifecycle
// model check, not a user trap, terminal, job-control or isolation test.
inline Status check_process_sessions_model(sched::SchedulerState &scheduler)
{
    const auto context = arch::configured_arch_operations().make_user_context({0x400000, 0x800000});
    std::array<sched::ProcessId, 2> owned{};
    const auto result = [&]() -> Status {
        const auto parent = scheduler.create_user_process("session-parent-model", context);
        if (!parent) return parent.status();
        owned[0] = parent.value();
        if (auto status = scheduler.set_runnable(parent.value()); !status) return status;
        const auto child = scheduler.fork_process(parent.value());
        if (!child) return child.status();
        owned[1] = child.value();
        const auto *process = scheduler.find(child.value());
        if (!process || process->process_group_id() != parent.value() ||
            process->session_id() != parent.value() || process->has_execed())
            return Status::fault("session fork inheritance model failed");
        const auto origin = user::process_signal_sender(*scheduler.find(parent.value()));
        usize selected = 0;
        const auto partial = user::send_process_group_signal<64>(scheduler, origin, 0, 0,
            [&](user::ProcessSignalSender sender, sched::ProcessId target, i32 signal) -> i64 {
                if (sender.pid != parent.value() || signal ||
                    (target != parent.value() && target != child.value())) return -22;
                ++selected;
                return target == parent.value() ? -1 : 0;
            });
        if (partial != 0 || selected != 2 ||
            user::send_process_group_signal<64>(scheduler, origin, 0x80000000U, 0,
                [](user::ProcessSignalSender, sched::ProcessId, i32) -> i64 { return 0; }) != -3 ||
            user::send_process_group_signal<64>(scheduler, origin, 0, 19,
                [](user::ProcessSignalSender, sched::ProcessId, i32) -> i64 { return 0; }) != -95)
            return Status::fault("bounded process group signal selector model failed");
        if (auto status = scheduler.set_process_group(parent.value(), static_cast<i32>(child.value()), 0); !status)
            return status;
        sched::MemoryMap memory;
        if (auto status = scheduler.replace_image(child.value(), context, memory); !status) return status;
        process = scheduler.find(child.value());
        if (!process || process->process_group_id() != child.value() ||
            process->session_id() != parent.value() || !process->has_execed() ||
            scheduler.set_process_group(parent.value(), static_cast<i32>(child.value()),
                                        static_cast<i32>(parent.value())).code() != StatusCode::denied)
            return Status::fault("session exec preservation and parent refusal model failed");
        if (auto status = scheduler.set_process_group(child.value(), 0, static_cast<i32>(parent.value())); !status)
            return status;
        const auto session = scheduler.create_session(child.value());
        if (!session || session.value() != child.value())
            return Status::fault("session creation model failed");
        const auto group = scheduler.process_group(parent.value(), static_cast<i32>(child.value()));
        const auto observed = scheduler.session_id(parent.value(), static_cast<i32>(child.value()));
        if (!group || !observed || group.value() != child.value() || observed.value() != child.value() ||
            scheduler.create_session(child.value()).status().code() != StatusCode::permission)
            return Status::fault("session leader publication model failed");
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
    return result;
}
} // namespace ok::tests
