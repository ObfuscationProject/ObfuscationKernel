#pragma once

#include "ok/sched/scheduler.hpp"
#include "ok/sched/child_exit.hpp"
#include "ok/user/native_signal_action.hpp"

namespace ok::user::native
{

// Stable private owner state. Never retain a PCB, TCB or pending-ticket pointer.
struct ChildExitLineage
{
    sched::ProcessId parent{};
    u64 parent_epoch{};
    sched::ThreadId parent_tid{};
    sched::ChildExitStatus leader_cause{};
};

template <abi::linux_user::Profile Profile>
struct ChildExitLifecycle
{
    using Actions = SignalActionCodec<Profile>;
    using State = sched::ProcessState;
    struct Completion
    {
        sched::ProcessId child{}, parent{};
        sched::ThreadId parent_tid{};
        u64 parent_epoch{};
        u32 uid{};
        sched::ChildExitStatus notification{}, wait_cause{};
        bool published{}, qualified_parent{}, notify{}, autoreap{}, consumed{};
    };
    struct Adoption
    {
        sched::ProcessId from{}, to{};
        u64 from_epoch{}, to_epoch{};
        sched::ThreadId to_tid{};
        StaticVector<sched::ProcessId, sched::max_process_children> live_children{};
    };
    enum class Notification : u8 { skipped, queued, coalesced };

    [[nodiscard]] static bool completed(const sched::ProcessControlBlock &process)
    { return process.state() == State::zombie || process.state() == State::exited; }

    template <typename Lookup>
    [[nodiscard]] static Status prepare_adoption(sched::SchedulerState &scheduler,
                                                  sched::ProcessId from, Lookup &&lookup,
                                                  Adoption &out)
    {
        out = {};
        const auto to = scheduler.user_adoption_target(from);
        if (!to) return Status::success();
        const auto *source = scheduler.find(from);
        const auto *target = scheduler.find(to);
        if (!source || !target) return Status::fault("init adoption owner missing");
        if (source->children().empty()) return Status::success();
        auto *old_owner = lookup(from);
        auto *new_owner = lookup(to);
        if (!owns(old_owner, from) || !owns(new_owner, to))
            return Status::fault("init adoption signal owner missing");
        sched::ThreadId target_tid{};
        for (const auto &thread : target->threads())
            if (thread.native_active && thread.state != State::exited &&
                (!target_tid || thread.tid < target_tid)) target_tid = thread.tid;
        if (!target_tid) return Status::fault("init adoption has no live signal thread");
        Adoption next{};
        next.from = from; next.to = to;
        next.from_epoch = old_owner->epoch;
        next.to_epoch = new_owner->epoch;
        next.to_tid = target_tid;
        for (const auto id : source->children())
        {
            const auto *child = scheduler.find(id);
            if (!child || child->parent_pid() != from)
                return Status::fault("init adoption child link missing");
            if (child->execution() != sched::ProcessExecution::user_process || completed(*child))
                continue;
            if (child->state() == State::created)
                return Status::busy("unpublished child cannot be adopted");
            auto *owner = lookup(id);
            if (!owns(owner, id) || owner->lineage.parent != from ||
                owner->lineage.parent_epoch != old_owner->epoch)
                return Status::fault("init adoption child lineage missing");
            if (auto status = next.live_children.push_back(id); !status) return status;
        }
        out = next;
        return Status::success();
    }

    template <typename Lookup>
    [[nodiscard]] static Status commit_adoption(sched::SchedulerState &scheduler,
                                                 const Adoption &adoption, Lookup &&lookup)
    {
        if (!adoption.to) return Status::success();
        const auto *target = scheduler.find(adoption.to);
        auto *new_owner = lookup(adoption.to);
        if (!target || completed(*target) || !owns(new_owner, adoption.to) ||
            new_owner->epoch != adoption.to_epoch ||
            !scheduler.find_thread(adoption.to, adoption.to_tid))
            return Status::fault("init adoption target changed");
        for (const auto id : adoption.live_children)
        {
            const auto *child = scheduler.find(id);
            auto *owner = lookup(id);
            if (!child || child->parent_pid() != adoption.to || completed(*child) ||
                !owns(owner, id) || owner->lineage.parent != adoption.from ||
                owner->lineage.parent_epoch != adoption.from_epoch)
                return Status::fault("init adoption transition changed");
            usize links = 0;
            for (const auto linked : target->children()) links += linked == id;
            if (links != 1) return Status::fault("init adoption target link changed");
        }
        for (const auto id : adoption.live_children)
        {
            auto *owner = lookup(id);
            owner->lineage.parent = adoption.to;
            owner->lineage.parent_epoch = adoption.to_epoch;
            owner->lineage.parent_tid = adoption.to_tid;
        }
        return Status::success();
    }

    template <typename Owner>
    [[nodiscard]] static bool owns(const Owner *owner, sched::ProcessId pid)
    {
        if (!owner || owner->pid != pid || !owner->epoch) return false;
        const auto identity = owner->shared.identity();
        return identity.process == pid && identity.thread == pid && identity.generation == owner->epoch;
    }

    template <typename Lookup>
    [[nodiscard]] static Status inherit(sched::SchedulerState &scheduler, sched::ProcessId parent,
                                        sched::ThreadId caller, sched::ProcessId child, Lookup &&lookup)
    {
        const auto *process = scheduler.find(child);
        const auto *thread = scheduler.find_thread(parent, caller);
        auto *from = lookup(parent);
        auto *to = lookup(child);
        if (!process || process->parent_pid() != parent || process->state() != State::created ||
            !thread || thread->state == State::exited || !thread->native_active ||
            !owns(from, parent) || !owns(to, child))
            return Status::fault("private child lineage fork owner");
        to->lineage = {parent, from->epoch, caller, {}};
        return Status::success();
    }

    template <typename Lookup, typename RealBlocked>
    [[nodiscard]] static Status prepare(sched::SchedulerState &scheduler, sched::ProcessId pid,
                                        sched::ChildExitStatus cause, Lookup &&lookup,
                                        RealBlocked &&real_blocked, Completion &out)
    {
        out = {};
        const auto *child = scheduler.find(pid);
        if (!child) return Status::not_found("private child completion owner");
        if (completed(*child)) return Status::already_exists("private child completion repeated");
        if (!cause.valid(Actions::max_signal)) return Status::invalid_argument("private child completion cause");
        Completion next{};
        next.child = pid; next.parent = child->parent_pid(); next.uid = child->credentials().uid;
        next.notification = next.wait_cause = cause;
        // Unpublished fork rollback remains owned by its explicit cleanup wait.
        if (child->state() == State::created || child->resources().personality != sched::UserPersonality::linux)
        { out = next; return Status::success(); }
        next.published = true;
        const auto *owner = lookup(pid);
        if (!owns(owner, pid)) return Status::fault("private child completion signal owner");
        if (owner->lineage.leader_cause != sched::ChildExitStatus{})
        {
            if (!owner->lineage.leader_cause.valid(Actions::max_signal))
                return Status::fault("private child completion saved cause");
            next.notification = owner->lineage.leader_cause;
        }
        if (!next.parent) { out = next; return Status::success(); }
        const auto *parent = scheduler.find(next.parent);
        if (!parent || completed(*parent)) return Status::fault("private child completion parent missing");
        // Native-personality parents retain their established wait contract.
        // They are not Linux signal recipients, even after a child execs musl.
        if (parent->resources().personality != sched::UserPersonality::linux)
        { out = next; return Status::success(); }
        const auto *recipient = lookup(next.parent);
        if (!owns(recipient, next.parent) ||
            owner->lineage.parent != next.parent || owner->lineage.parent_epoch != recipient->epoch)
            return Status::fault("private child completion parent lineage");
        usize links = 0;
        for (const auto id : parent->children()) links += id == pid;
        const auto *thread = scheduler.find_thread(next.parent, owner->lineage.parent_tid);
        if (links != 1 || !thread || !thread->native_active || thread->state == State::exited)
            return Status::fault("private child completion named parent thread");
        next.parent_epoch = recipient->epoch; next.parent_tid = thread->tid; next.qualified_parent = true;
        const auto *action = recipient->table.find(Actions::child_signal);
        if (!action) return Status::fault("private child completion disposition");
        const bool ignored = action->handler == sched::SignalAction::ignored_handler;
        next.autoreap = ignored || (action->flags & Actions::no_child_wait);
        const auto *real = real_blocked(*thread);
        next.notify = !ignored && (action->handler != sched::SignalAction::default_handler ||
            thread->native_signal_mask.contains(Actions::child_signal) ||
            (real && real->contains(Actions::child_signal)));
        out = next;
        return Status::success();
    }

    template <typename Lookup>
    [[nodiscard]] static Status commit(sched::SchedulerState &scheduler, Completion &completion,
                                       Lookup &&lookup, Notification &notification)
    {
        notification = Notification::skipped;
        if (completion.consumed) return Status::already_exists("private child completion already committed");
        // A failed commit is terminal for this serialized service operation.
        completion.consumed = true;
        const auto *child = scheduler.find(completion.child);
        if (!child || !completed(*child) || child->parent_pid() != completion.parent)
            return Status::fault("private child completion transition missing");
        if (!completion.published || !completion.parent) return Status::success();
        const auto *parent = scheduler.find(completion.parent);
        if (!parent || completed(*parent)) return Status::fault("private child completion parent changed");
        if (!completion.qualified_parent)
            return parent->resources().personality != sched::UserPersonality::linux &&
                !completion.notify && !completion.autoreap && !completion.parent_tid && !completion.parent_epoch
                ? Status::success() : Status::fault("private child completion parent policy changed");
        auto *recipient = lookup(completion.parent);
        if (!parent || completed(*parent) || !owns(recipient, completion.parent) ||
            recipient->epoch != completion.parent_epoch)
            return Status::fault("private child completion parent changed");
        usize links = 0;
        for (const auto id : parent->children()) links += id == completion.child;
        if (links != 1) return Status::fault("private child completion child link changed");
        if (completion.notify)
        {
            const auto result = recipient->shared.enqueue_child_exit(completion.notification,
                static_cast<u32>(completion.child), completion.uid);
            using QueueResult = decltype(result);
            if (result == QueueResult::queued) notification = Notification::queued;
            else if (result == QueueResult::coalesced) notification = Notification::coalesced;
            else return Status::fault("private child completion pending capacity");
        }
        if (completion.autoreap)
            return scheduler.detach_completed_child_to_reaper(completion.parent, completion.child);
        return Status::success();
    }

    template <typename Lookup>
    [[nodiscard]] static Status rebind(sched::SchedulerState &scheduler, sched::ProcessId pid,
                                       sched::ThreadId departed, sched::ThreadId replacement, Lookup &&lookup)
    {
        const auto *parent = scheduler.find(pid);
        const auto *owner = lookup(pid);
        const auto *thread = scheduler.find_thread(pid, replacement);
        if (!parent || completed(*parent) || !owns(owner, pid) || !thread ||
            thread->state == State::exited || !thread->native_active)
            return Status::fault("private child lineage replacement owner");
        // Validate the complete batch before editing any child lineage.
        for (const auto child_pid : parent->children())
        {
            const auto *child = scheduler.find(child_pid);
            if (!child || child->parent_pid() != pid) return Status::fault("private child lineage child link");
            if (completed(*child)) continue;
            const auto *state = lookup(child_pid);
            if (!state && child->resources().personality != sched::UserPersonality::linux) continue;
            if (!owns(state, child_pid) || state->lineage.parent != pid || state->lineage.parent_epoch != owner->epoch)
                return Status::fault("private child lineage replacement child");
        }
        for (const auto child_pid : parent->children())
        {
            const auto *child = scheduler.find(child_pid);
            if (completed(*child)) continue;
            auto *state = lookup(child_pid);
            if (!state) continue;
            if (!departed || state->lineage.parent_tid == departed) state->lineage.parent_tid = replacement;
        }
        return Status::success();
    }

    template <typename Lookup>
    [[nodiscard]] static Status thread_exited(sched::SchedulerState &scheduler, sched::ProcessId pid,
                                              sched::ThreadId tid, sched::ChildExitStatus cause, Lookup &&lookup)
    {
        const auto *process = scheduler.find(pid);
        const auto *thread = scheduler.find_thread(pid, tid);
        auto *owner = lookup(pid);
        if (!process || !thread || thread->state != State::exited || !owns(owner, pid) ||
            !cause.valid(Actions::max_signal)) return Status::fault("private child lineage thread completion");
        if (tid == pid)
        {
            if (owner->lineage.leader_cause.valid(Actions::max_signal))
                return Status::already_exists("private child leader cause repeated");
            owner->lineage.leader_cause = cause;
        }
        sched::ThreadId replacement{};
        for (const auto &peer : process->threads())
            if (peer.state != State::exited && peer.native_active && (!replacement || peer.tid < replacement))
                replacement = peer.tid;
        return replacement ? rebind(scheduler, pid, tid, replacement, lookup) : Status::success();
    }

    template <typename Lookup>
    [[nodiscard]] static Status exec_committed(sched::SchedulerState &scheduler, sched::ProcessId pid, Lookup &&lookup)
    {
        auto *owner = lookup(pid);
        if (!owns(owner, pid)) return Status::fault("private child lineage exec owner");
        if (auto status = rebind(scheduler, pid, 0, pid, lookup); !status) return status;
        owner->lineage.leader_cause = {};
        return Status::success();
    }
};

// Cleanup and transition failures cannot publish an event or detach a child.
template <typename Cleanup, typename Transition, typename Commit>
[[nodiscard]] Status child_exit_transaction(Cleanup &&cleanup, Transition &&transition, Commit &&commit)
{
    if (auto status = cleanup(); !status) return status;
    if (auto status = transition(); !status) return status;
    return commit();
}

} // namespace ok::user::native
