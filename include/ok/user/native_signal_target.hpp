#pragma once

#include "ok/sched/scheduler.hpp"
#include "ok/sched/pending_signals.hpp"
#include "ok/user/native_signal_action.hpp"

namespace ok::user
{

// Linux do_sigpending returns pending intersected with the calling thread's
// blocked set. Runtime callers union directed and shared process pending
// queues before this operation. Short and zero-sized
// copies are permitted by rt_sigpending, unlike rt_sigprocmask's exact size.
template <abi::linux_user::Profile Profile, typename Write>
[[nodiscard]] i64 query_directed_signal_pending(sched::SignalMask pending,
                                               sched::SignalMask blocked,
                                               u64 destination, u64 size, Write &&write)
{
    using Codec = native::SignalMaskCodec<Profile>;
    if (size > Codec::bytes) return -22;
    if (!size) return 0;
    for (usize index = 0; index < pending.words.size(); ++index)
        pending.words[index] &= blocked.words[index];
    const auto bytes = Codec::encode(pending);
    return std::forward<Write>(write)(destination,
        std::span<const std::byte>{bytes.data(), static_cast<usize>(size)}) ? 0 : -14;
}

// Linux 6.13.4 kernel/signal.c: kill_ok_by_cred/check_kill_permission.
// Effective root is this kernel's reduced CAP_KILL policy. This does not model
// capabilities, user namespaces, LSM or the SIGCONT session exception.
[[nodiscard]] constexpr bool directed_signal_permitted(Credentials sender, Credentials target,
                                                       bool same_thread_group)
{
    return same_thread_group || sender.euid == root_uid ||
           sender.euid == target.uid || sender.euid == target.suid ||
           sender.uid == target.uid || sender.uid == target.suid;
}

struct DirectedSignalTarget
{
    sched::ProcessControlBlock *process{};
    sched::ThreadControlBlock *thread{};
    bool retired_leader{};

    [[nodiscard]] explicit operator bool() const { return process && thread; }
};

// The caller holds the existing serialized boot-CPU execution boundary.
// Pointers are borrowed only until the next scheduler/compaction boundary.
// A zero group selects tkill's global TID domain; tgkill supplies a positive
// owner. Signal validation/permission occurs after successful lookup, as in
// Linux do_send_specific. Looking up a target never acquires delivery state.
[[nodiscard]] inline DirectedSignalTarget find_directed_signal_target(
    sched::SchedulerState &scheduler, sched::ProcessId group, sched::ThreadId tid)
{
    if (!tid) return {};
    for (auto &process : scheduler.processes())
    {
        if (group && process.pid() != group) continue;
        for (auto &thread : process.threads())
        {
            if (thread.tid != tid || thread.owner != process.pid()) continue;
            const bool retired = thread.state == sched::ProcessState::exited;
            // The group leader remains addressable until wait/reap even after
            // its native entry resources are gone. Retired nonleaders do not.
            if (retired && tid != process.pid()) return {};
            return {&process, &thread, retired};
        }
    }
    return {};
}

// The callback owns actual target action/queue acquisition. In particular it is
// never called by a zero probe, a refused request or a retained retired leader.
// This is shared by the native boundary and scheduler-backed model tests.
struct RejectDirectedBlockedWait
{
    bool operator()(const sched::ProcessControlBlock &, const sched::ThreadControlBlock &) const { return false; }
};

template <abi::linux_user::Profile Profile, typename Deliver, typename AdmitBlocked = RejectDirectedBlockedWait>
[[nodiscard]] i64 send_directed_signal(sched::SchedulerState &scheduler,
                                      const sched::ProcessControlBlock &sender,
                                      bool tkill, const std::array<u64, 6> &arguments,
                                      Deliver &&deliver, AdmitBlocked &&admit_blocked = {})
{
    using Codec = native::SignalActionCodec<Profile>;
    const i32 group = tkill ? 0 : static_cast<i32>(arguments[0]);
    const i32 tid = static_cast<i32>(arguments[tkill ? 0 : 1]);
    const i32 signal = static_cast<i32>(arguments[tkill ? 1 : 2]);
    if (tid <= 0 || (!tkill && group <= 0)) return -22;
    const auto target = find_directed_signal_target(scheduler, static_cast<u32>(group),
                                                   static_cast<u32>(tid));
    if (!target) return -3;
    if (signal < 0 || signal > static_cast<i32>(Codec::max_signal)) return -22;
    const auto credentials = sender.credentials();
    if (!directed_signal_permitted(credentials, target.process->credentials(),
                                   sender.pid() == target.process->pid())) return -1;
    if (!signal || target.retired_leader) return 0;
    if (!target.thread->native_active || target.process->resources().personality != sched::UserPersonality::linux)
        return -95;
    // Only a caller with a qualified wait continuation may admit blocked tasks.
    // The default remains refusal; signal masks and blocked waits are distinct.
    if ((target.thread->state == sched::ProcessState::blocked &&
         !std::forward<AdmitBlocked>(admit_blocked)(*target.process, *target.thread)) ||
        signal == static_cast<i32>(Codec::stop_signal) ||
        signal == (Codec::mips ? 24 : 20) || signal == (Codec::mips ? 26 : 21) ||
        signal == (Codec::mips ? 27 : 22) || signal == (Codec::mips ? 25 : 18)) return -95;
    return std::forward<Deliver>(deliver)(*target.process, *target.thread,
        sched::PendingSignalInfo{static_cast<u32>(signal), -6,
                                 static_cast<u32>(sender.pid()), credentials.uid});
}

// Accessors bind actions to the recipient PID and pending state to its current
// TCB entry generation. Keep acquisition lazy: an ignored unmasked signal does
// not acquire/reset a pending queue. Accessors must not schedule or move PCBs.
template <abi::linux_user::Profile Profile, typename Actions, typename Pending>
[[nodiscard]] i64 enqueue_directed_signal(sched::ProcessControlBlock &recipient,
                                         sched::ThreadControlBlock &thread,
                                         sched::PendingSignalInfo info,
                                         Actions &&actions_for, Pending &&pending_for)
{
    using Codec = native::SignalActionCodec<Profile>;
    if (thread.owner != recipient.pid() || !info.signal || info.signal > Codec::max_signal) return -22;
    auto *actions = std::forward<Actions>(actions_for)(recipient.pid());
    if (!actions) return -12;
    if (Codec::ignored(info.signal, *actions->find(info.signal)) && !thread.native_signal_mask.contains(info.signal))
        return 0;
    auto *pending = std::forward<Pending>(pending_for)(thread);
    if (!pending) return -12;
    using Queue = std::remove_reference_t<decltype(*pending)>;
    const auto result = pending->enqueue(info);
    return result == Queue::Enqueue::queued || result == Queue::Enqueue::coalesced ? 0 : -11;
}

} // namespace ok::user
