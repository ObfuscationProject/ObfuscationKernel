#include "../../src/user/signals/child_exit_lifecycle.hpp"
#include "../../src/user/signals/process_pending.hpp"
#include "../../src/user/signals/wait_state.hpp"
#include "ok/abi/linux_native.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ok;
using namespace ok::sched;
using namespace ok::user::native;
static usize checks, before_calls, after_calls, fail_before, fail_after;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (false)
namespace ok::user::native {
Status private_signal_before_process_compaction(SchedulerState &)
{ return ++before_calls == fail_before ? Status::fault("injected pre-erase failure") : Status::success(); }
Status private_signal_after_process_compaction(SchedulerState &)
{ return ++after_calls == fail_after ? Status::fault("injected post-erase failure") : Status::success(); }
}
constexpr auto profile = abi::linux_user::configured_native_profile();
using Lifecycle = ChildExitLifecycle<profile>;
using Actions = Lifecycle::Actions;
constexpr u32 child_signal = Actions::child_signal;

template <u64 MaximumSequence = ~u64{0}>
struct Fixture
{
    using Queue = ProcessPendingSignals<Actions::max_signal, 32, MaximumSequence>;
    struct Owner
    {
        ProcessId pid{}; u64 epoch{}; SignalActionTable table{}; Queue shared{};
        ChildExitLineage lineage{};
    };
    std::unique_ptr<Scheduler> storage{std::make_unique<Scheduler>()};
    Scheduler &scheduler{*storage};
    std::unique_ptr<std::array<Owner, max_processes>> owner_storage{std::make_unique<std::array<Owner, max_processes>>()};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(0x1000, 0x8000)};
    SignalMask real{}; ThreadId real_tid{};
    Fixture() { before_calls = after_calls = fail_before = fail_after = 0; }
    Owner *find(ProcessId pid)
    { for (auto &owner : *owner_storage) if (owner.pid == pid) return &owner; return nullptr; }
    auto lookup() { return [this](ProcessId pid) { return find(pid); }; }
    Owner &reserve(ProcessId pid)
    {
        for (auto &owner : *owner_storage) if (!owner.pid)
        {
            owner.pid = pid; ++owner.epoch; owner.table = {}; owner.lineage = {};
            CHECK(owner.shared.reset({pid, pid, owner.epoch})); return owner;
        }
        std::abort();
    }
    void retire(ProcessId pid)
    {
        if (auto *owner = find(pid)) { owner->shared.retire(); owner->pid = 0; owner->lineage = {}; }
    }
    ProcessId parent()
    {
        const auto result = scheduler.create_process("child-exit", context); CHECK(result);
        auto *process = scheduler.find(result.value());
        process->resources().personality = UserPersonality::linux;
        process->threads()[0].native_active = true;
        auto credentials = process->credentials(); credentials.uid = 0xfedcba98; process->set_credentials(credentials);
        CHECK(scheduler.set_runnable(result.value())); reserve(result.value()); return result.value();
    }
    ThreadId peer(ProcessId pid)
    {
        const auto result = scheduler.create_thread(pid, context); CHECK(result);
        scheduler.find_thread(pid, result.value())->native_active = true; return result.value();
    }
    ProcessId child(ProcessId parent, ThreadId caller = 0, bool publish = true, bool inherit = true)
    {
        if (!caller) caller = parent;
        CHECK(scheduler.activate_thread(parent, caller));
        const auto result = scheduler.fork_process(parent, false); CHECK(result);
        auto &owner = reserve(result.value());
        if (inherit) CHECK(Lifecycle::inherit(scheduler, parent, caller, result.value(), lookup()));
        owner.table = find(parent)->table.fork_copy();
        if (publish)
        {
            scheduler.find_thread(result.value(), result.value())->native_active = true;
            CHECK(scheduler.set_runnable(result.value()));
        }
        return result.value();
    }
    void action(ProcessId pid, u64 handler, u64 flags = 0)
    { *find(pid)->table.find(child_signal) = {handler, flags, 0, {}}; }
    void blocked(ProcessId pid, ThreadId tid, bool yes)
    { scheduler.find_thread(pid, tid)->native_signal_mask.set(child_signal, yes); }
    Status prepare(ProcessId pid, ChildExitStatus cause, Lifecycle::Completion &completion)
    {
        return Lifecycle::prepare(scheduler, pid, cause, lookup(), [&](const ThreadControlBlock &thread) -> const SignalMask * {
            return thread.tid == real_tid ? &real : nullptr;
        }, completion);
    }
    Status transition(ProcessId pid, ChildExitStatus cause)
    {
        for (auto &thread : scheduler.find(pid)->threads()) thread.native_active = false;
        retire(pid);
        return cause.code() == 1 ? scheduler.exit_process(pid, cause.status())
                                : scheduler.terminate_process(pid, static_cast<u8>(cause.status()));
    }
    Lifecycle::Notification complete(ProcessId pid, ChildExitStatus cause = ChildExitStatus::normal(23))
    {
        Lifecycle::Completion completion; CHECK(prepare(pid, cause, completion));
        Lifecycle::Notification notification{};
        CHECK(child_exit_transaction([] { return Status::success(); }, [&] { return transition(pid, cause); },
            [&] { return Lifecycle::commit(scheduler, completion, lookup(), notification); }));
        return notification;
    }
    typename Queue::Delivery pending(ProcessId pid)
    { typename Queue::Delivery ticket; CHECK(find(pid)->shared.peek({}, ticket)); return ticket; }
};

void matrix()
{
    for (u64 disposition = 0; disposition != 3; ++disposition)
        for (bool blocked : {false, true}) for (bool real : {false, true})
            for (bool no_wait : {false, true}) for (bool killed : {false, true})
            {
                Fixture f; const auto parent = f.parent();
                f.action(parent, disposition, no_wait ? Actions::no_child_wait : 0);
                f.blocked(parent, parent, blocked);
                if (real) { f.real_tid = parent; f.real.set(child_signal); }
                const auto child = f.child(parent);
                const auto *child_address = f.scheduler.find(child);
                const auto *parent_address = f.scheduler.find(parent);
                const auto cause = killed ? ChildExitStatus::signaled(15) : ChildExitStatus::normal(0x123);
                const auto notification = f.complete(child, cause);
                const bool notified = disposition != 1 && (disposition == 2 || blocked || real);
                const bool autoreap = disposition == 1 || no_wait;
                CHECK(notification == (notified ? Lifecycle::Notification::queued : Lifecycle::Notification::skipped));
                CHECK(f.scheduler.find(parent) == parent_address); CHECK(f.scheduler.find(child) == child_address);
                CHECK(before_calls == 0 && after_calls == 0);
                CHECK(child_address->parent_pid() == (autoreap ? 0 : parent));
                CHECK(f.scheduler.find(parent)->children().size() == (autoreap ? 0 : 1));
                CHECK(child_address->wait_status() == (killed ? 15 : 0x2300));
                if (notified)
                {
                    const auto ticket = f.pending(parent);
                    CHECK(ticket.info == (PendingSignalInfo{child_signal, cause.code(), static_cast<u32>(child),
                                                           0xfedcba98, cause.status()}));
                }
                Lifecycle::Completion duplicate;
                CHECK(!f.prepare(child, cause, duplicate));
                OrphanReapProgress progress; CHECK(f.scheduler.reap_orphans_checked(progress));
                CHECK(progress.reaped == static_cast<usize>(autoreap));
                if (!autoreap) CHECK(f.scheduler.wait_process(parent, child));
                CHECK(!f.scheduler.find(child));
                CHECK(f.find(parent)->shared.pending_mask().contains(child_signal) == notified);
            }
}

void independent_lifetimes()
{
    Fixture f; const auto parent = f.parent(); f.action(parent, 2);
    const auto first = f.child(parent), second = f.child(parent);
    CHECK(f.complete(first, ChildExitStatus::normal(17)) == Lifecycle::Notification::queued);
    CHECK(f.complete(second, ChildExitStatus::signaled(15)) == Lifecycle::Notification::coalesced);
    auto ticket = f.pending(parent); CHECK(ticket.info.sender_pid == first && ticket.info.child_status == 17);
    CHECK(f.scheduler.wait_process(parent, first)); CHECK(f.scheduler.wait_process(parent, second));
    CHECK(f.pending(parent).info == ticket.info); CHECK(f.find(parent)->shared.consume(ticket));
    const auto third = f.child(parent); f.complete(third); ticket = f.pending(parent);
    CHECK(ticket.info.sender_pid == third); CHECK(f.find(parent)->shared.consume(ticket));
    CHECK(f.scheduler.find(third)); CHECK(f.scheduler.wait_process(parent, third));
    // Existing SI_USER work keeps the first payload independently of child wait.
    CHECK(f.find(parent)->shared.enqueue({child_signal, 0, 919, 42}) == Fixture<>::Queue::Enqueue::queued);
    const auto fourth = f.child(parent); f.complete(fourth);
    CHECK(f.pending(parent).info == (PendingSignalInfo{child_signal, 0, 919, 42}));
    CHECK(f.scheduler.wait_process(parent, fourth));
    f.find(parent)->shared.discard(child_signal);
    // Installing ignored/NOCLDWAIT does not sweep old waitable children.
    const auto old = f.child(parent); f.complete(old);
    f.action(parent, 1); f.find(parent)->shared.discard(child_signal);
    CHECK(f.scheduler.find(old)->parent_pid() == parent);
    const auto newer = f.child(parent); f.complete(newer);
    CHECK(f.scheduler.find(newer)->parent_pid() == 0); CHECK(f.scheduler.wait_process(parent, old));
}

void publication_and_failures()
{
    Fixture f; const auto parent = f.parent(); f.action(parent, 2, Actions::no_child_wait);
    // Missing lineage/owner is allowed only while the fork is unpublished.
    const auto rollback = f.child(parent, parent, false, false); f.retire(rollback);
    CHECK(f.complete(rollback) == Lifecycle::Notification::skipped);
    CHECK(f.scheduler.find(rollback)->parent_pid() == parent); CHECK(f.scheduler.wait_process(parent, rollback));
    CHECK(!f.find(parent)->shared.pending_mask().contains(child_signal));
    const auto child = f.child(parent); Lifecycle::Completion completion;
    CHECK(!f.prepare(child, ChildExitStatus{}, completion));
    const auto lineage = f.find(child)->lineage;
    f.find(child)->lineage.parent_epoch++; CHECK(!f.prepare(child, ChildExitStatus::normal(23), completion));
    f.find(child)->lineage = lineage;
    f.find(child)->lineage.parent_tid = 999; CHECK(!f.prepare(child, ChildExitStatus::normal(23), completion));
    f.find(child)->lineage = lineage;
    CHECK(f.prepare(child, ChildExitStatus::normal(23), completion));
    usize transition = 0, commit = 0;
    CHECK(!child_exit_transaction([] { return Status::fault("cleanup"); },
        [&] { ++transition; return Status::success(); }, [&] { ++commit; return Status::success(); }));
    CHECK(transition == 0 && commit == 0);
    CHECK(!child_exit_transaction([] { return Status::success(); },
        [&] { ++transition; return Status::fault("transition"); }, [&] { ++commit; return Status::success(); }));
    CHECK(transition == 1 && commit == 0);
    CHECK(f.transition(child, ChildExitStatus::normal(23)));
    f.find(parent)->epoch++;
    Lifecycle::Notification notification;
    CHECK(!Lifecycle::commit(f.scheduler, completion, f.lookup(), notification));
    CHECK(f.scheduler.find(child)->parent_pid() == parent);
    CHECK(!f.find(parent)->shared.pending_mask().contains(child_signal));
    CHECK(!Lifecycle::commit(f.scheduler, completion, f.lookup(), notification));
}

void named_parent_and_exec()
{
    Fixture f; const auto parent = f.parent(), caller = f.peer(parent), other = f.peer(parent);
    f.blocked(parent, parent, false); f.blocked(parent, caller, true); f.blocked(parent, other, false);
    const auto first = f.child(parent, caller); Lifecycle::Completion completion;
    CHECK(f.prepare(first, ChildExitStatus::normal(23), completion)); CHECK(completion.notify);
    CHECK(completion.parent_tid == caller);
    // The named creator is replaced by the lowest live TID, not an arbitrary mask.
    CHECK(f.scheduler.exit_thread(parent, caller));
    CHECK(Lifecycle::thread_exited(f.scheduler, parent, caller, ChildExitStatus::normal(9), f.lookup()));
    CHECK(f.find(first)->lineage.parent_tid == parent);
    CHECK(f.prepare(first, ChildExitStatus::normal(23), completion)); CHECK(!completion.notify);
    const auto second = f.child(parent, other);
    f.scheduler.find(second)->resources().personality = UserPersonality::native;
    f.find(parent)->lineage.leader_cause = ChildExitStatus::normal(17);
    CHECK(f.scheduler.replace_image(parent, f.context, {}));
    f.scheduler.find_thread(parent, parent)->native_active = true;
    CHECK(Lifecycle::exec_committed(f.scheduler, parent, f.lookup()));
    CHECK(f.find(first)->lineage.parent_tid == parent && f.find(second)->lineage.parent_tid == parent);
    CHECK(!f.find(parent)->lineage.leader_cause.valid(Actions::max_signal));
    // A corrupt child prevents partial lineage edits during a batch.
    const auto third = f.child(parent); f.find(third)->lineage.parent_epoch++;
    const auto peer = f.peer(parent);
    CHECK(!Lifecycle::rebind(f.scheduler, parent, 0, peer, f.lookup()));
    CHECK(f.find(first)->lineage.parent_tid == parent && f.find(second)->lineage.parent_tid == parent);
}

void leader_cause_and_orphans()
{
    for (const auto final : {ChildExitStatus::normal(23), ChildExitStatus::signaled(15), ChildExitStatus::signaled(9)})
    {
        Fixture f; const auto parent = f.parent(); f.action(parent, 2);
        const auto child = f.child(parent); const auto peer = f.peer(child);
        CHECK(f.scheduler.exit_thread(child, child));
        CHECK(Lifecycle::thread_exited(f.scheduler, child, child, ChildExitStatus::normal(17), f.lookup()));
        CHECK(!f.find(parent)->shared.pending_mask().contains(child_signal));
        CHECK(f.scheduler.exit_thread(child, peer));
        CHECK(Lifecycle::thread_exited(f.scheduler, child, peer, final, f.lookup()));
        f.complete(child, final);
        const auto ticket = f.pending(parent); CHECK(ticket.info.code == 1 && ticket.info.child_status == 17);
        CHECK(f.scheduler.find(child)->wait_status() == (final.code() == 1 ? 23 * 256 : final.status()));
    }
    Fixture f; const auto parent = f.parent(), child = f.child(parent);
    f.complete(parent);
    CHECK(f.scheduler.find(child)->parent_pid() == 0);
    OrphanReapProgress progress; CHECK(f.scheduler.reap_orphans_checked(progress)); CHECK(progress.reaped == 1);
    const auto successor = f.parent(); // Reuses a private owner slot with a new epoch.
    f.action(successor, 2); CHECK(f.complete(child) == Lifecycle::Notification::skipped);
    CHECK(!f.find(successor)->shared.pending_mask().contains(child_signal));
    CHECK(f.scheduler.reap_orphans_checked(progress)); CHECK(progress.reaped == 1);
}

void pending_capacity_and_reap_faults()
{
    Fixture<1> f; const auto parent = f.parent(); f.action(parent, 2, Actions::no_child_wait);
    const auto first = f.child(parent); f.complete(first);
    const auto ticket = f.pending(parent); CHECK(f.find(parent)->shared.consume(ticket));
    const auto second = f.child(parent); Lifecycle::Completion completion;
    CHECK(f.prepare(second, ChildExitStatus::normal(23), completion)); CHECK(f.transition(second, ChildExitStatus::normal(23)));
    Lifecycle::Notification notification; CHECK(!Lifecycle::commit(f.scheduler, completion, f.lookup(), notification));
    CHECK(f.scheduler.find(second)->parent_pid() == parent);
    fail_before = before_calls + 1; OrphanReapProgress progress;
    CHECK(!f.scheduler.reap_orphans_checked(progress)); CHECK(progress.reaped == 0 && progress.failed_pid == first);
    fail_before = 0; fail_after = after_calls + 1;
    CHECK(!f.scheduler.reap_orphans_checked(progress)); CHECK(progress.reaped == 1 && progress.failed_after_erase);
    CHECK(!f.scheduler.find(first));
    // Realtime saturation has no bearing on the standard SIGCHLD slot.
    Fixture rt; const auto p = rt.parent(); rt.action(p, 2);
    for (usize i = 0; i < 32; ++i)
        CHECK(rt.find(p)->shared.enqueue({32, 0, 1, 0}) == Fixture<>::Queue::Enqueue::queued);
    const auto c = rt.child(p); CHECK(rt.complete(c) == Lifecycle::Notification::queued);
    CHECK(rt.pending(p).info.signal == child_signal);
}

void designated_init_adoption()
{
    Fixture f;
    const auto worker = f.scheduler.create_process("early-kernel-worker", f.context);
    CHECK(worker && worker.value() != user_init_pid);
    const auto user_context = arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0});
    const auto created_init = f.scheduler.create_init_process(user_context);
    CHECK(created_init && created_init.value() == user_init_pid);
    CHECK(!f.scheduler.create_init_process(user_context));
    auto *init = f.scheduler.find(user_init_pid);
    init->resources().personality = UserPersonality::linux;
    init->threads()[0].native_active = true;
    CHECK(f.scheduler.set_runnable(user_init_pid));
    f.reserve(user_init_pid);

    const auto created_parent = f.scheduler.create_user_process("service", user_context);
    CHECK(created_parent);
    const auto parent = created_parent.value();
    auto *process = f.scheduler.find(parent);
    process->resources().personality = UserPersonality::linux;
    process->threads()[0].native_active = true;
    CHECK(f.scheduler.set_runnable(parent));
    f.reserve(parent);
    const auto child = f.child(parent);
    CHECK(f.scheduler.user_adoption_target(parent) == user_init_pid);
    Lifecycle::Adoption adoption;
    CHECK(Lifecycle::prepare_adoption(f.scheduler, parent, f.lookup(), adoption));
    CHECK(adoption.to == user_init_pid && adoption.live_children.size() == 1);
    auto *departing = f.scheduler.find(parent);
    CHECK(departing->children().push_back(child));
    CHECK(!f.scheduler.exit_process(parent, 17));
    CHECK(departing->state() != ProcessState::zombie &&
          f.scheduler.find(child)->parent_pid() == parent);
    CHECK(departing->children().erase_at(departing->children().size() - 1));
    CHECK(f.transition(parent, ChildExitStatus::normal(17)));
    CHECK(f.scheduler.find(child)->parent_pid() == user_init_pid);
    CHECK(Lifecycle::commit_adoption(f.scheduler, adoption, f.lookup()));
    CHECK(f.find(child)->lineage.parent == user_init_pid);
    CHECK(f.find(child)->lineage.parent_epoch == f.find(user_init_pid)->epoch);
    Lifecycle::Completion completed;
    CHECK(f.prepare(child, ChildExitStatus::normal(23), completed));
    CHECK(completed.qualified_parent && completed.parent == user_init_pid);
    CHECK(f.transition(child, ChildExitStatus::normal(23)));
    Lifecycle::Notification notice{};
    CHECK(Lifecycle::commit(f.scheduler, completed, f.lookup(), notice));
    CHECK(f.scheduler.wait_process(user_init_pid, child));
    OrphanReapProgress progress;
    CHECK(f.scheduler.reap_orphans_checked(progress));
    CHECK(progress.reaped == 1 && f.scheduler.find(user_init_pid));
    CHECK(f.scheduler.exit_process(user_init_pid, 0));
    CHECK(f.scheduler.reap_orphans_checked(progress));
    CHECK(progress.reaped == 0 && f.scheduler.find(user_init_pid));
    CHECK(!f.scheduler.reap_process(user_init_pid));
}

void unpublished_init_fallback()
{
    Fixture f;
    const auto user_context = arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0});
    const auto init = f.scheduler.create_init_process(user_context);
    CHECK(init && init.value() == user_init_pid);
    f.scheduler.find(user_init_pid)->resources().personality = UserPersonality::linux;
    const auto service = f.scheduler.create_user_process("early-service", user_context);
    CHECK(service);
    const auto parent = service.value();
    auto *process = f.scheduler.find(parent);
    process->resources().personality = UserPersonality::linux;
    process->threads()[0].native_active = true;
    CHECK(f.scheduler.set_runnable(parent));
    f.reserve(parent);
    const auto child = f.child(parent);
    CHECK(f.scheduler.user_adoption_target(parent) == 0);
    Lifecycle::Adoption adoption;
    CHECK(Lifecycle::prepare_adoption(f.scheduler, parent, f.lookup(), adoption));
    CHECK(adoption.to == 0 && adoption.live_children.empty());
    CHECK(f.transition(parent, ChildExitStatus::normal(17)));
    CHECK(f.scheduler.find(child)->parent_pid() == 0);
}

int main()
{
    matrix(); independent_lifetimes(); publication_and_failures(); named_parent_and_exec();
    leader_cause_and_orphans(); pending_capacity_and_reap_faults(); designated_init_adoption();
    unpublished_init_fallback();
    std::printf("PASS child exit lifecycle checks=%zu guest=0 smp=0\n", checks);
}
