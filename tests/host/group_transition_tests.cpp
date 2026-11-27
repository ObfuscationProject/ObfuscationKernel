// Host-thread models of the group transitions (spec 4.7-4.9 and 4.12) on the
// real task, task table, run queue and kernel locks (OK_HOSTED_LOCK_MODEL):
// quiesce_group with siblings in user mode while the claimer holds the big
// kernel lock, the exec-failure thaw, exit beats exec, a second exec, a kill
// of an all-parked process, SIGCONT with a pending probe, fork against a
// group kill and stop/continue against wakes. Host threads stand in for
// CPUs; no AP starts, no IPI is sent and no guest runs (scope=model).
#include "ok/sched/group_quiesce.hpp"
#include "ok/smp/kernel_lock.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#if !defined(OK_HOSTED_LOCK_MODEL)
#error "the group transition models need the real spin locks"
#endif

using namespace ok;
using namespace ok::sched;
namespace
{
std::atomic<usize> checks{};
#define CHECK(condition)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        checks.fetch_add(1, std::memory_order_relaxed);                                                               \
        if (!static_cast<bool>(condition))                                                                            \
        {                                                                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                      \
            std::abort();                                                                                             \
        }                                                                                                             \
    } while (false)

constexpr usize cpus = 4;
using Outcome = SchedulerState::ClaimOutcome;

struct Fixture
{
    std::unique_ptr<Scheduler> owned{std::make_unique<Scheduler>()};
    Scheduler &scheduler{*owned};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(0x1000, 0x8000)};

    Fixture()
    {
        CHECK(scheduler.configure_cpus(cpus));
        CHECK(scheduler.set_online_cpus((1u << cpus) - 1));
        scheduler.set_native_userland(true);
    }
    ProcessId process()
    {
        auto pid = scheduler.create_user_process("group", context);
        CHECK(pid);
        thread(pid.value()).native_active = true;
        CHECK(scheduler.set_runnable(pid.value()));
        return pid.value();
    }
    ThreadId add_thread(ProcessId pid)
    {
        auto tid = scheduler.create_thread_prepared(pid, context, [](ThreadControlBlock &created) {
            created.native_active = true;
            return Status::success();
        });
        CHECK(tid);
        return tid.value();
    }
    ThreadControlBlock &thread(ProcessId pid, ThreadId tid = 0)
    {
        auto *found = scheduler.find_thread(pid, tid ? tid : pid);
        CHECK(found);
        return *found;
    }
    usize entries(ProcessId pid, ThreadId tid) const
    {
        usize count = 0;
        for (smp::CpuId cpu = 0; cpu < cpus; ++cpu) count += scheduler.queued_on(cpu, {pid, tid});
        return count;
    }
    // Picks on `cpu` until `tid` of `pid` runs there (other entries go back).
    void run(smp::CpuId cpu, ProcessId pid, ThreadId tid)
    {
        const smp::ScopedHostedCpu hosted{cpu};
        for (usize attempt = 0; attempt < 64; ++attempt)
        {
            const auto picked = scheduler.pick(cpu);
            if (picked && picked->ref == (ThreadRef{pid, tid})) return;
            if (picked) scheduler.put_prev(cpu);
        }
        CHECK(!"thread not picked");
    }
    // Parks the thread `cpu` runs in a wait and returns its ticket.
    WaitTicket park(smp::CpuId cpu, ThreadWaitReason reason, u64 key)
    {
        const smp::ScopedHostedCpu hosted{cpu};
        auto ticket = scheduler.register_current_wait(reason, key);
        CHECK(ticket);
        scheduler.put_prev(cpu);
        return ticket.value();
    }
};

void expect_clean_checks()
{
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu) CHECK(smp::lock_violation_count(cpu) == 0);
    CHECK(smp::check_total(smp::KernelCheck::queue) == 0);
    CHECK(smp::check_total(smp::KernelCheck::pin) == 0);
    CHECK(smp::check_total(smp::KernelCheck::audit_repairs) == 0);
    CHECK(smp::check_total(smp::KernelCheck::quiesce_timeouts) == 0);
    CHECK(smp::check_total(smp::KernelCheck::class_rule) == 0);
}

// The model's side of quiesce_group: the real kernel lock, a yield for the
// interrupt service, the host clock. Counts the waits that released the lock.
struct ModelHost
{
    std::atomic<usize> *released{};
    bool kernel_lock_held() const { return smp::kernel_lock.owned_by_current_cpu(); }
    void release_kernel_lock()
    {
        if (released) released->fetch_add(1, std::memory_order_relaxed);
        smp::bkl_unlock();
    }
    void retake_kernel_lock() { smp::bkl_lock(); }
    void serve() { std::this_thread::yield(); }
    u64 now_ns() const
    {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    }
};
static_assert(QuiesceHost<ModelHost>);

Scheduler *frozen_scheduler{};
bool model_frozen(u64 pid) noexcept { return frozen_scheduler && frozen_scheduler->frozen(pid); }

// Exec with siblings in user mode while the claimer holds the kernel lock
// (spec 4.8; Appendix A 1-C1). CPU 0 runs the step under the kernel lock and
// claims exec for whichever thread of the group it picked; CPUs 1-3 run the
// step for the other threads: pick under the kernel lock, user mode without
// it, then the post-user relock and its frozen check, which skips dispatch
// and goes to put_prev. The claimer's wait releases the kernel lock; without
// that the siblings could never reach put_prev. Each round ends with the
// exec-failure thaw: every sibling is queued again, none was lost.
void exec_quiesces_user_siblings()
{
    Fixture f;
    const auto pid = f.process();
    const ThreadId tids[] = {pid, f.add_thread(pid), f.add_thread(pid), f.add_thread(pid)};
    frozen_scheduler = &f.scheduler;
    smp::bkl_frozen_check = model_frozen;
    constexpr usize rounds = 4000;
    std::atomic<bool> stop{false};
    std::atomic<usize> frozen_skips{0}, dispatches{0}, released{0}, waited{0};
    std::vector<std::thread> siblings;
    for (smp::CpuId cpu = 1; cpu < cpus; ++cpu)
        siblings.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu hosted{cpu};
            usize spin = cpu * 97;
            while (!stop.load(std::memory_order_acquire))
            {
                smp::bkl_lock();
                const auto picked = f.scheduler.pick(cpu);
                // Pick refuses a frozen group under the same lock the claim took.
                if (picked) CHECK(!f.scheduler.frozen(pid));
                smp::bkl_unlock();
                if (!picked)
                {
                    std::this_thread::yield();
                    continue;
                }
                // User mode: bounded (a timer tick or the claimer's kick ends it).
                spin = (spin * 1103515245u + 12345u) % 4096;
                for (usize i = 0; i < spin; ++i) std::atomic_signal_fence(std::memory_order_seq_cst);
                smp::bkl_lock(); // The post-user relock (enter_unlocked).
                if (smp::bkl_thread_frozen(pid))
                    frozen_skips.fetch_add(1, std::memory_order_relaxed);
                else
                    dispatches.fetch_add(1, std::memory_order_relaxed);
                f.scheduler.put_prev(cpu);
                smp::bkl_unlock();
            }
        });
    {
        const smp::ScopedHostedCpu hosted{0};
        ModelHost host{&released};
        usize round = 0;
        while (round < rounds)
        {
            smp::bkl_lock();
            const auto picked = f.scheduler.pick(0);
            if (!picked)
            {
                smp::bkl_unlock();
                std::this_thread::yield();
                continue;
            }
            ++round;
            const auto claimer = picked->ref.tid;
            if (!f.scheduler.group_quiesced(pid, claimer)) waited.fetch_add(1, std::memory_order_relaxed);
            CHECK(quiesce_group(f.scheduler, pid, claimer, GroupTransition::exec, host) == QuiesceResult::quiesced);
            // Holding the kernel lock again: no sibling runs or is queued.
            CHECK(smp::kernel_lock.owned_by_current_cpu() && f.scheduler.frozen(pid));
            CHECK(f.scheduler.group_quiesced(pid, claimer));
            for (const auto tid : tids)
                if (tid != claimer)
                {
                    CHECK(f.thread(pid, tid).on_cpu == no_cpu && f.entries(pid, tid) == 0);
                    CHECK(f.thread(pid, tid).queued_cpu == no_cpu);
                }
            // A second exec of the group, or an exit, finds the claim.
            CHECK(f.scheduler.claim_transition(pid, tids[claimer == tids[1] ? 2 : 1], GroupTransition::exec).outcome ==
                  Outcome::exec_held);
            // The exec fails: the group thaws and every sibling is queued again.
            f.scheduler.unfreeze(pid, claimer);
            CHECK(!f.scheduler.frozen(pid) && f.scheduler.transition(pid) == GroupTransition::none);
            for (const auto tid : tids)
                if (tid != claimer) CHECK(f.entries(pid, tid) == 1 && f.thread(pid, tid).on_cpu == no_cpu);
            f.scheduler.put_prev(0);
            smp::bkl_unlock();
            // The siblings' picks and user stretches overlap the next claim.
            for (usize i = 0; i < (round % 7) * 600; ++i) std::atomic_signal_fence(std::memory_order_seq_cst);
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto &sibling : siblings) sibling.join();
    for (const auto tid : tids)
    {
        CHECK(f.thread(pid, tid).on_cpu == no_cpu && f.entries(pid, tid) == 1);
        CHECK(f.thread(pid, tid).state == ProcessState::runnable);
    }
    CHECK(f.scheduler.audit_queues() == 0);
    smp::bkl_frozen_check = nullptr;
    frozen_scheduler = nullptr;
    expect_clean_checks();
    // The kernel lock was released for siblings in user mode, and some of
    // them came back to find their group frozen.
    CHECK(waited.load() > 0 && released.load() > 0 && frozen_skips.load() > 0 && dispatches.load() > 0);
    std::printf("exec_quiesce rounds=%zu waited=%zu released=%zu frozen_skips=%zu dispatches=%zu\n", rounds,
                waited.load(), released.load(), frozen_skips.load(), dispatches.load());
}

// A sibling held in user mode by CPU 1 until the test lets it return.
struct HeldSibling
{
    Fixture &f;
    ProcessId pid;
    ThreadId tid;
    std::atomic<bool> release{false};
    std::atomic<bool> frozen_seen{false};
    std::thread cpu;
    HeldSibling(Fixture &fixture, ProcessId process, ThreadId thread)
        : f(fixture), pid(process), tid(thread),
          cpu([this] {
              const smp::ScopedHostedCpu hosted{1};
              while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
              smp::bkl_lock();
              frozen_seen.store(smp::bkl_thread_frozen(pid), std::memory_order_relaxed);
              f.scheduler.put_prev(1);
              smp::bkl_unlock();
          })
    {
    }
    ~HeldSibling()
    {
        release.store(true, std::memory_order_release);
        if (cpu.joinable()) cpu.join();
    }
};

// Exit beats exec (spec 4.8): a kill lands while the exec claimer waits with
// the kernel lock released. The exec's commit check finds the pending exit
// and its claim becomes an exit claim; the claimer's finish sees it held.
// A second exec of the group is refused while the first runs (double exec).
void exit_beats_exec()
{
    Fixture f;
    const auto pid = f.process();
    const auto sibling = f.add_thread(pid);
    frozen_scheduler = &f.scheduler;
    smp::bkl_frozen_check = model_frozen;
    f.run(1, pid, sibling); // In user mode on CPU 1.
    f.run(0, pid, pid);
    std::atomic<usize> released{0};
    {
        HeldSibling held{f, pid, sibling};
        std::thread killer([&] {
            const smp::ScopedHostedCpu hosted{2};
            // A cross-process kill: under the kernel lock the claimer released.
            while (released.load(std::memory_order_acquire) == 0) std::this_thread::yield();
            smp::bkl_lock();
            CHECK(f.scheduler.claim_transition(pid, sibling, GroupTransition::exec).outcome == Outcome::exec_held);
            CHECK(f.scheduler.request_group_exit(pid, {0, 9}) == 0); // The exec claim runs: no probe, no kick.
            smp::bkl_unlock();
            held.release.store(true, std::memory_order_release);
        });
        const smp::ScopedHostedCpu hosted{0};
        ModelHost host{&released};
        smp::bkl_lock();
        CHECK(quiesce_group(f.scheduler, pid, pid, GroupTransition::exec, host) == QuiesceResult::quiesced);
        killer.join();
        held.cpu.join();
        CHECK(held.frozen_seen.load());
        const auto superseded = f.scheduler.exec_superseded(pid, pid);
        CHECK(superseded && superseded->signal == 9);
        CHECK(f.scheduler.transition(pid) == GroupTransition::exit && f.scheduler.frozen(pid));
        CHECK(f.scheduler.claim_transition(pid, pid, GroupTransition::exit).outcome == Outcome::held);
        CHECK(f.scheduler.claim_transition(pid, sibling, GroupTransition::exit).outcome == Outcome::exit_held);
        // The frozen sibling is not queued; the group is finished in place.
        CHECK(f.entries(pid, sibling) == 0 && f.thread(pid, sibling).on_cpu == no_cpu);
        CHECK(f.scheduler.exit_process(pid, 0));
        f.scheduler.put_prev(0);
        smp::bkl_unlock();
    }
    // An exit wanted while an exec runs sets pending_exit; the exec claimer
    // aborts at its commit check.
    const auto other = f.process();
    const auto peer = f.add_thread(other);
    CHECK(f.scheduler.claim_transition(other, other, GroupTransition::exec).outcome == Outcome::claimed);
    CHECK(f.scheduler.claim_transition(other, peer, GroupTransition::exit, {3, 0}).outcome == Outcome::exit_pending);
    CHECK(f.scheduler.exit_pending(other));
    const auto request = f.scheduler.exec_superseded(other, other);
    CHECK(request && request->code == 3 && request->signal == 0);
    smp::bkl_frozen_check = nullptr;
    frozen_scheduler = nullptr;
    expect_clean_checks();
    std::printf("exit_beats_exec released=%zu\n", released.load());
}

// Kill of an all-parked process (spec 4.8): every thread waits off its CPU.
// The kill sets pending_exit, probes each thread and queues it while
// completions of some of those waits race it on other CPUs. Each thread ends
// queued exactly once, and pick admits it in whatever state it is: blocked
// ones probed, so the delivery point runs before any wait resumes.
void kill_all_parked()
{
    constexpr usize rounds = 2000;
    usize blocked_admitted = 0;
    for (usize round = 0; round < rounds; ++round)
    {
        Fixture f;
        const auto pid = f.process();
        const ThreadId tids[] = {pid, f.add_thread(pid), f.add_thread(pid)};
        std::array<WaitTicket, 3> tickets{};
        for (usize i = 0; i < 3; ++i)
        {
            f.run(static_cast<smp::CpuId>(i), pid, tids[i]);
            tickets[i] = f.park(static_cast<smp::CpuId>(i), ThreadWaitReason::futex, 0x1000 + i);
        }
        std::atomic<bool> go{false};
        std::thread waker([&] {
            const smp::ScopedHostedCpu hosted{1};
            while (!go.load(std::memory_order_acquire)) {}
            static_cast<void>(f.scheduler.complete_wait(tickets[round % 3], WaitCompletion::ready));
        });
        std::thread killer([&] {
            const smp::ScopedHostedCpu hosted{2};
            while (!go.load(std::memory_order_acquire)) {}
            CHECK(f.scheduler.request_group_exit(pid, {0, 9}) == 0); // Nothing ran: no kick.
        });
        go.store(true, std::memory_order_release);
        waker.join();
        killer.join();
        CHECK(f.scheduler.exit_pending(pid));
        for (usize i = 0; i < 3; ++i)
        {
            const auto &thread = f.thread(pid, tids[i]);
            CHECK(f.entries(pid, tids[i]) == 1 && thread.on_cpu == no_cpu);
            CHECK(thread.state == ProcessState::runnable || (thread.state == ProcessState::blocked && thread.probe));
        }
        // Pick admits every thread, blocked or not.
        const smp::ScopedHostedCpu hosted{3};
        for (usize i = 0; i < 3; ++i)
        {
            const auto picked = f.scheduler.pick(3);
            CHECK(picked && picked->ref.pid == pid);
            blocked_admitted += picked->probed;
            f.scheduler.put_prev(3);
            CHECK(f.scheduler.exit_pending(pid));
        }
        const auto request = f.scheduler.pending_exit(pid);
        CHECK(request && request->signal == 9);
    }
    expect_clean_checks();
    CHECK(blocked_admitted > 0);
    std::printf("kill_all_parked rounds=%zu blocked_admitted=%zu\n", rounds, blocked_admitted);
}

// SIGCONT with a pending probe (spec 4.9): a probe set while the process is
// stopped keeps no queue entry; the continue queues the thread and pick
// admits it blocked. Raced: probes on one CPU, stop and continue on another.
void sigcont_with_probe()
{
    {
        Fixture f;
        const auto pid = f.process();
        f.run(0, pid, pid);
        f.park(0, ThreadWaitReason::pipe_read, 7);
        CHECK(f.scheduler.stop_process(pid, 19));
        CHECK(f.scheduler.probe_parked(*f.scheduler.find(pid), f.thread(pid)));
        CHECK(f.thread(pid).probe && f.entries(pid, pid) == 0);
        CHECK(!f.scheduler.pick(1));
        CHECK(f.scheduler.audit_queues() == 0); // A stopped group is not an orphan.
        CHECK(f.scheduler.continue_process(pid));
        CHECK(f.entries(pid, pid) == 1);
        const smp::ScopedHostedCpu hosted{1};
        const auto picked = f.scheduler.pick(1);
        CHECK(picked && picked->probed && f.thread(pid).state == ProcessState::blocked);
        f.scheduler.put_prev(1);
    }
    constexpr usize rounds = 4000;
    usize stopped_first = 0;
    Fixture f;
    const auto pid = f.process();
    f.run(0, pid, pid);
    auto ticket = f.park(0, ThreadWaitReason::pipe_read, 9);
    for (usize round = 0; round < rounds; ++round)
    {
        std::atomic<bool> go{false};
        std::atomic<bool> probed{false}, unqueued{false};
        std::thread prober([&] {
            const smp::ScopedHostedCpu hosted{1};
            while (!go.load(std::memory_order_acquire)) {}
            probed.store(f.scheduler.probe_parked(*f.scheduler.find(pid), f.thread(pid)), std::memory_order_relaxed);
            // Probed while stopped: no entry until the continue.
            const auto pinned = f.scheduler.pin(pid);
            unqueued.store(pinned && pinned.slot()->queued_cpu[0].load(std::memory_order_acquire) == no_cpu,
                           std::memory_order_relaxed);
        });
        std::thread job([&] {
            const smp::ScopedHostedCpu hosted{2};
            while (!go.load(std::memory_order_acquire)) {}
            CHECK(f.scheduler.stop_process(pid, 19));
            if (round % 2) std::this_thread::yield();
            CHECK(f.scheduler.continue_process(pid));
        });
        go.store(true, std::memory_order_release);
        prober.join();
        job.join();
        CHECK(probed.load());
        // Continued: the probed thread is queued exactly once, whichever ran first.
        CHECK(f.entries(pid, pid) == 1 && f.thread(pid).probe);
        stopped_first += unqueued.load();
        const smp::ScopedHostedCpu hosted{3};
        const auto picked = f.scheduler.pick(3);
        CHECK(picked && picked->probed);
        // The resume finds no signal: the wait parks again, unprobed.
        f.scheduler.put_prev(3);
        CHECK(f.entries(pid, pid) == 0 && f.thread(pid).wait_registration.pending(ticket));
    }
    expect_clean_checks();

    std::printf("sigcont_with_probe rounds=%zu probed_while_stopped=%zu\n", rounds, stopped_first);
}

// fork against kill(-pgrp) (spec 4.12): the killer enumerates the group and
// sets pending_exit on each member under the task table; the forker creates
// a child and publishes it under the task table after checking its parent.
// Whatever the order, no published child of a killed parent escapes: each
// one either was enumerated (it has pending_exit) or its publish saw the
// parent's pending exit and was undone.
void fork_versus_group_kill()
{
    constexpr usize rounds = 2000;
    usize refused = 0, killed_children = 0;
    for (usize round = 0; round < rounds; ++round)
    {
        Fixture f;
        const auto parent = f.process();
        std::atomic<bool> go{false};
        std::atomic<ProcessId> child{0};
        Status published{};
        std::thread forker([&] {
            const smp::ScopedHostedCpu hosted{0};
            while (!go.load(std::memory_order_acquire)) {}
            auto made = f.scheduler.fork_process(parent, false);
            CHECK(made);
            f.thread(made.value()).native_active = true;
            child.store(made.value(), std::memory_order_release);
            published = f.scheduler.publish_fork(parent, made.value());
        });
        std::thread killer([&] {
            const smp::ScopedHostedCpu hosted{1};
            while (!go.load(std::memory_order_acquire)) {}
            if (round % 3 == 0) std::this_thread::yield();
            const TaskTableLock::Guard table{f.scheduler.task_table_lock()};
            std::array<ProcessId, max_processes> members{};
            usize count = 0;
            for (const auto &process : f.scheduler.processes())
                if (process.process_group_id() == f.scheduler.find(parent)->process_group_id())
                    members[count++] = process.pid();
            for (usize i = 0; i < count; ++i) static_cast<void>(f.scheduler.request_group_exit(members[i], {0, 9}));
        });
        go.store(true, std::memory_order_release);
        forker.join();
        killer.join();
        CHECK(f.scheduler.exit_pending(parent));
        const auto id = child.load();
        if (!published)
        {
            CHECK(published.code() == StatusCode::interrupted);
            ++refused;
            CHECK(f.scheduler.exit_process(id, 127)); // The caller undoes the child.
        }
        else
        {
            CHECK(f.scheduler.exit_pending(id));
            ++killed_children;
        }
    }
    expect_clean_checks();
    CHECK(refused > 0 && killed_children > 0);
    std::printf("fork_vs_group_kill rounds=%zu refused=%zu killed_children=%zu\n", rounds, refused, killed_children);
}

// stop/continue against wakes (spec 4.9): CPU 1 runs the thread and parks it
// in a wait, CPU 2 completes its waits, CPU 3 stops and continues the
// process. A stopped process is never picked, and once everything settles
// and the process is continued, nothing was lost: the audit finds no
// runnable thread without an entry and no completed wait still blocked.
void stop_continue_versus_wake()
{
    Fixture f;
    const auto pid = f.process();
    std::atomic<bool> stop{false};
    std::atomic<u64> armed{0};
    std::atomic<usize> picks{0}, wakes{0}, stops{0};
    std::atomic<WaitTicket *> latest{nullptr};
    std::array<WaitTicket, 2> slots{};
    std::thread runner([&] {
        const smp::ScopedHostedCpu hosted{1};
        usize index = 0;
        while (!stop.load(std::memory_order_acquire))
        {
            const auto picked = f.scheduler.pick(1);
            if (!picked) continue;
            picks.fetch_add(1, std::memory_order_relaxed);
            if (picked->probed)
            {
                f.scheduler.put_prev(1);
                continue;
            }
            auto ticket = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, 3);
            CHECK(ticket);
            slots[index] = ticket.value();
            latest.store(&slots[index], std::memory_order_release);
            index ^= 1;
            f.scheduler.put_prev(1);
        }
    });
    std::thread waker([&] {
        const smp::ScopedHostedCpu hosted{2};
        while (!stop.load(std::memory_order_acquire))
            if (auto *ticket = latest.exchange(nullptr, std::memory_order_acq_rel))
                wakes.fetch_add(f.scheduler.complete_wait(*ticket, WaitCompletion::ready), std::memory_order_relaxed);
    });
    std::thread job([&] {
        const smp::ScopedHostedCpu hosted{3};
        while (!stop.load(std::memory_order_acquire))
        {
            CHECK(f.scheduler.stop_process(pid, 19));
            stops.fetch_add(1, std::memory_order_relaxed);
            for (int spin = 0; spin < 64; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
            CHECK(f.scheduler.continue_process(pid));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop.store(true, std::memory_order_release);
    runner.join();
    waker.join();
    job.join();
    const auto &thread = f.thread(pid);
    CHECK(thread.on_cpu == no_cpu);
    if (thread.state == ProcessState::runnable) CHECK(f.entries(pid, pid) == 1);
    else CHECK(thread.state == ProcessState::blocked && thread.wait_registration.active());
    CHECK(f.scheduler.audit_queues() == 0);
    expect_clean_checks();
    CHECK(picks.load() > 100 && wakes.load() > 100 && stops.load() > 100);
    std::printf("stop_continue_vs_wake picks=%zu wakes=%zu stops=%zu\n", picks.load(), wakes.load(), stops.load());
}
} // namespace

int main()
{
    exec_quiesces_user_siblings();
    exit_beats_exec();
    kill_all_parked();
    sigcont_with_probe();
    fork_versus_group_kill();
    stop_continue_versus_wake();
    std::printf("OK_GROUP_TRANSITION scope=model checks=%zu model_cpus=%zu guest=0 hardware_smp=0\n", checks.load(),
                cpus);
}
