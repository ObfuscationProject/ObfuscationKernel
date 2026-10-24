#pragma once

#include "ok/core/kernel.hpp"
#include "ok/user/robust_futex.hpp"

namespace ok::user::linux_bootstrap
{

template <abi::linux_user::Profile Profile>
i64 robust_registration(Kernel &kernel, sched::ProcessId pid, bool set, const std::array<u64, 6> &arguments)
{
    using Codec = native::RobustListCodec<Profile>;
    auto *process = kernel.scheduler().find(pid);
    auto *caller = kernel.scheduler().current_thread();
    if (!process || !caller || caller->owner != pid || caller->state != sched::ProcessState::running)
        return -22;
    if (set)
    {
        if (arguments[1] != Codec::head_bytes) return -22;
        caller->tls.robust_list = static_cast<uptr>(arguments[0]);
        caller->tls.robust_list_length = Codec::head_bytes;
        return 0;
    }
    const i32 requested = static_cast<i32>(arguments[0]);
    const sched::ThreadControlBlock *target = requested == 0 ? caller : nullptr;
    if (requested > 0)
        for (const auto &owner : kernel.scheduler().processes())
        {
            if (owner.execution() != sched::ProcessExecution::user_process ||
                owner.resources().personality != sched::UserPersonality::linux)
                continue;
            for (const auto &thread : owner.threads())
                if (thread.tid == static_cast<u32>(requested) &&
                    (thread.state != sched::ProcessState::exited || thread.tid == owner.pid()))
                    target = &thread;
        }
    if (!target) return -3;
    // Saved IDs, dumpability and ptrace capabilities are not represented yet.
    // Cross-group queries remain an explicit compatibility gap, including root.
    if (target->owner != pid) return -1;
    const u64 head = target->tls.robust_list;
    auto put = [&](u64 address, u64 value) {
        std::array<u8, Codec::word_bytes> bytes{};
        for (usize i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<u8>(value >> (8 * (Codec::big_endian ? bytes.size() - 1 - i : i)));
        return arch::execution::copy_to_user(process->resources().native_space,
            static_cast<uptr>(address), bytes.data(), bytes.size());
    };
    // The first successful store remains visible if the second faults or aliases.
    if (!put(arguments[2], Codec::head_bytes)) return -14;
    return put(arguments[1], head) ? 0 : -14;
}

} // namespace ok::user::linux_bootstrap
