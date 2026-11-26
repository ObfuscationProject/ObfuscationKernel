// Deadlines (smp-scaling WP4b, spec 4.6): the indexed min-heap against a
// reference model, its full 576-key capacity, and the scheduler's use of it
// on the real spin locks (OK_HOSTED_LOCK_MODEL, OK_LOCK_DEBUG): one deadline
// per wait, removal on completion, the timeout, retry and probe actions,
// process keys, batched expiry and expiry racing completion on host threads.
// Host threads stand in for CPUs; no guest runs (scope=model).
#include "ok/sched/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#if !defined(OK_HOSTED_LOCK_MODEL)
#error "the deadline races need the real spin locks"
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
using Heap = DeadlineHeap<deadline_keys>;
constexpr auto never = Heap::never;

void expect_clean_checks()
{
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu) CHECK(smp::lock_violation_count(cpu) == 0);
    CHECK(smp::check_total(smp::KernelCheck::queue) == 0);
    CHECK(smp::check_total(smp::KernelCheck::pin) == 0);
    CHECK(smp::check_total(smp::KernelCheck::audit_repairs) == 0);
}

// Random set, move, remove and pop against a reference array: the earliest,
// every key's deadline, the size and the heap invariants agree after every
// operation, and pops come out in deadline order.
void heap_against_model()
{
    auto heap = std::make_unique<Heap>();
    std::array<std::optional<u64>, deadline_keys> model{};
    std::mt19937_64 random{0x5eed};
    usize pops = 0, moves = 0;
    for (usize operation = 0; operation < 200000; ++operation)
    {
        const auto key = static_cast<u16>(random() % deadline_keys);
        const u64 deadline = random() % 5000;
        switch (random() % 4)
        {
        case 0:
        case 1:
            moves += model[key].has_value();
            CHECK(heap->set(key, deadline));
            model[key] = deadline;
            break;
        case 2:
            CHECK(heap->remove(key) == model[key].has_value());
            model[key].reset();
            break;
        default:
        {
            const u64 now = random() % 5000;
            u64 lowest = never;
            for (const auto &entry : model)
                if (entry && *entry < lowest) lowest = *entry;
            const auto popped = heap->pop_due(now);
            CHECK(popped.has_value() == (lowest <= now));
            if (popped)
            {
                CHECK(model[*popped] && *model[*popped] == lowest);
                model[*popped].reset();
                ++pops;
            }
        }
        }
        if (operation % 97 == 0)
        {
            CHECK(heap->consistent());
            usize present = 0;
            u64 lowest = never;
            for (usize k = 0; k < deadline_keys; ++k)
            {
                CHECK(heap->contains(static_cast<u16>(k)) == model[k].has_value());
                CHECK(heap->when(static_cast<u16>(k)) == (model[k] ? *model[k] : never));
                present += model[k].has_value();
                if (model[k] && *model[k] < lowest) lowest = *model[k];
            }
            CHECK(heap->size() == present && heap->earliest() == lowest);
        }
    }
    // Draining pops every key in deadline order.
    u64 previous = 0;
    while (const auto popped = heap->pop_due(never - 1))
    {
        CHECK(model[*popped] && *model[*popped] >= previous);
        previous = *model[*popped];
        model[*popped].reset();
    }
    CHECK(heap->empty() && heap->earliest() == never && heap->consistent());
    for (const auto &entry : model) CHECK(!entry);
    CHECK(pops > 1000 && moves > 1000);
    std::printf("heap_model operations=200000 pops=%zu moves=%zu\n", pops, moves);
}

// Every key at once: 512 thread keys and 64 process keys fit; a set of a
// present key moves it and never grows the heap; out-of-range keys are refused.
void heap_capacity()
{
    auto heap = std::make_unique<Heap>();
    for (usize key = 0; key < deadline_keys; ++key) CHECK(heap->set(static_cast<u16>(key), deadline_keys - key));
    CHECK(heap->size() == deadline_keys && heap->consistent() && heap->earliest() == 1);
    for (usize key = 0; key < deadline_keys; key += 3) CHECK(heap->set(static_cast<u16>(key), 10000 + key));
    CHECK(heap->size() == deadline_keys && heap->consistent());
    CHECK(!heap->set(static_cast<u16>(deadline_keys), 1) && !heap->remove(static_cast<u16>(deadline_keys)));
    usize popped = 0;
    while (const auto key = heap->pop_due(never - 1))
    {
        CHECK(heap->when(*key) == never && heap->consistent());
        ++popped;
    }
    CHECK(popped == deadline_keys && heap->empty());
}

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
        auto pid = scheduler.create_user_process("deadline", context);
        CHECK(pid);
        thread(pid.value()).native_active = true;
        CHECK(scheduler.set_runnable(pid.value()));
        return pid.value();
    }
    ThreadControlBlock &thread(ProcessId pid) { return *scheduler.find_thread(pid, pid); }
    u16 key(ProcessId pid) { return scheduler.find(pid)->thread_index(thread(pid)); }
    void run(ProcessId pid, smp::CpuId cpu = 0)
    {
        const auto picked = scheduler.pick(cpu);
        CHECK(picked && picked->ref.pid == pid);
    }
    // Arms a wait of `pid` on CPU 0 and parks it.
    WaitTicket park(ProcessId pid, ThreadWaitReason reason = ThreadWaitReason::deadline)
    {
        run(pid);
        auto wait = scheduler.register_current_wait(reason, 1);
        CHECK(wait);
        scheduler.put_prev(0);
        return wait.value();
    }
    usize expire(u64 now)
    {
        std::array<u16, max_processes> due{};
        return scheduler.expire_deadlines(now, due);
    }
};

// One deadline per wait (the earlier of two arms); completion removes it;
// expiry completes the wait as timed out and queues the parked thread; a
// stale or completed wait is not armed.
void scheduler_wait_deadlines()
{
    Fixture f;
    const smp::ScopedHostedCpu cpu{0};
    const auto pid = f.process();
    auto wait = f.park(pid);
    const auto key = f.key(pid);
    CHECK(f.scheduler.arm_wait_deadline(wait, 500));
    CHECK(f.scheduler.arm_wait_deadline(wait, 700)); // Later: the earlier stays.
    CHECK(f.scheduler.deadline_of(key) == 500 && f.scheduler.earliest_deadline() == 500);
    CHECK(f.scheduler.arm_wait_deadline(wait, 300)); // Earlier: it moves.
    CHECK(f.scheduler.deadline_of(key) == 300 && f.scheduler.earliest_deadline() == 300);
    CHECK(f.expire(299) == 0 && f.thread(pid).wait_registration.pending(wait));
    CHECK(f.expire(300) == 0);
    CHECK(f.thread(pid).wait_registration.completion() == WaitCompletion::timeout);
    CHECK(f.thread(pid).state == ProcessState::runnable && f.thread(pid).queued_cpu != no_cpu);
    CHECK(f.scheduler.deadline_of(key) == never && f.scheduler.earliest_deadline() == never);
    CHECK(f.scheduler.wakes(WakeSource::timer) == 1);
    // A completed wait cannot be armed; a completion removes the deadline.
    CHECK(!f.scheduler.arm_wait_deadline(wait, 900) && f.scheduler.earliest_deadline() == never);
    wait = f.park(pid);
    CHECK(f.scheduler.arm_wait_deadline(wait, 400));
    CHECK(f.scheduler.complete_wait(wait, WaitCompletion::ready));
    CHECK(f.scheduler.deadline_of(key) == never && f.scheduler.earliest_deadline() == never);
    // A wait cancelled without complete_wait (its registration directly) can
    // leave its key: the expiry's ticket then completes nothing.
    wait = f.park(pid);
    CHECK(f.scheduler.arm_wait_deadline(wait, 450));
    CHECK(f.scheduler.complete_wait(wait, WaitCompletion::cancelled));
    auto next = f.park(pid);
    CHECK(f.scheduler.arm_wait_deadline(next, 1000) && f.scheduler.deadline_of(key) == 1000);
    CHECK(f.expire(600) == 0 && f.thread(pid).wait_registration.pending(next));
    CHECK(f.scheduler.complete_wait(next, WaitCompletion::ready));
    expect_clean_checks();
}

// The retry action completes the wait as ready (a wait without a channel
// entry re-evaluates); the probe action leaves it pending, sets its probe and
// queues the parked thread so that its owner re-checks it (spec 4.3 A5).
void scheduler_actions()
{
    Fixture f;
    const smp::ScopedHostedCpu cpu{0};
    const auto pid = f.process();
    auto wait = f.park(pid, ThreadWaitReason::pipe_read);
    CHECK(f.scheduler.arm_wait_deadline(wait, 100, DeadlineAction::retry));
    CHECK(f.expire(100) == 0);
    CHECK(f.thread(pid).wait_registration.completion() == WaitCompletion::ready);
    CHECK(f.thread(pid).state == ProcessState::runnable);
    wait = f.park(pid, ThreadWaitReason::signal_wait);
    CHECK(f.scheduler.arm_wait_deadline(wait, 200, DeadlineAction::probe));
    CHECK(f.expire(200) == 0);
    const auto &thread = f.thread(pid);
    CHECK(thread.wait_registration.pending(wait) && thread.state == ProcessState::blocked);
    CHECK(thread.probe && thread.queued_cpu != no_cpu);
    // The probed pick admits it blocked; its owner then settles the wait.
    const auto picked = f.scheduler.pick(0);
    CHECK(picked && picked->probed && !f.thread(pid).probe);
    CHECK(f.scheduler.complete_wait(wait, WaitCompletion::timeout));
    CHECK(f.scheduler.resume_current(0));
    f.scheduler.put_prev(0);
    // A probe of a wait that is not pending changes nothing.
    CHECK(!f.scheduler.probe_wait(wait));
    expect_clean_checks();
}

// Process keys (spec 4.6): the timer service's slots come back to the caller;
// more than one batch of due keys expire in one call.
void scheduler_process_keys()
{
    Fixture f;
    const smp::ScopedHostedCpu cpu{0};
    for (usize slot = 0; slot < 8; ++slot) f.scheduler.set_process_deadline(slot, 100 + slot);
    f.scheduler.set_process_deadline(9, 50);
    f.scheduler.set_process_deadline(9, Heap::never); // Removed again.
    CHECK(f.scheduler.earliest_deadline() == 100);
    std::array<u16, max_processes> due{};
    CHECK(f.scheduler.expire_deadlines(103, due) == 4);
    std::sort(due.begin(), due.begin() + 4);
    CHECK(due[0] == 0 && due[1] == 1 && due[2] == 2 && due[3] == 3);
    CHECK(f.scheduler.earliest_deadline() == 104);
    CHECK(f.scheduler.expire_deadlines(1000, due) == 4 && f.scheduler.earliest_deadline() == never);
    // Forty parked waits due at once: one call expires them all (8 batches of 32).
    std::vector<ProcessId> pids;
    for (usize i = 0; i < 40; ++i) pids.push_back(f.process());
    for (usize parked = 0; parked < pids.size(); ++parked)
    {
        // Whichever is queued next: parked threads leave the queues.
        const auto picked = f.scheduler.pick(0);
        CHECK(picked);
        auto wait = f.scheduler.register_current_wait(ThreadWaitReason::deadline, 1);
        CHECK(wait && f.scheduler.arm_wait_deadline(wait.value(), 2000));
        f.scheduler.put_prev(0);
    }
    // Expired 5 ms late: the lateness is recorded (OK_SMP_WAKE deadline_late_max_us).
    CHECK(f.expire(2000 + 5'000'000) == 0 && f.scheduler.earliest_deadline() == never);
    for (const auto pid : pids) CHECK(f.thread(pid).wait_registration.completion() == WaitCompletion::timeout);
    CHECK(smp::evidence_max(&smp::EvidenceCpu::deadline_late_max_us) == 5000);
    // Keys due on every step (0: a timer awaiting its signal's dequeue) on
    // all but one process slot: the last slot's ordinary expiry is handed out
    // in the same call, not deferred behind them, and the 0 keys record no
    // lateness (they would claim the whole uptime).
    for (usize slot = 0; slot + 1 < max_processes; ++slot) f.scheduler.set_process_deadline(slot, 0);
    f.scheduler.set_process_deadline(max_processes - 1, 3'000'000'000);
    for (usize round = 0; round < 3; ++round)
    {
        std::array<u16, max_processes> due{};
        CHECK(f.scheduler.expire_deadlines(3'000'000'000 + round, due) == (round == 0 ? max_processes : max_processes - 1));
        std::sort(due.begin(), due.begin() + (round == 0 ? max_processes : max_processes - 1));
        for (usize slot = 0; slot + 1 < max_processes; ++slot) CHECK(due[slot] == slot);
        if (round == 0) CHECK(due[max_processes - 1] == max_processes - 1);
        // The caller's service sets them again; the timer slot has expired.
        for (usize slot = 0; slot + 1 < max_processes; ++slot) f.scheduler.set_process_deadline(slot, 0);
    }
    CHECK(smp::evidence_max(&smp::EvidenceCpu::deadline_late_max_us) == 5000);
    for (usize slot = 0; slot < max_processes; ++slot) f.scheduler.set_process_deadline(slot, Heap::never);
    CHECK(f.scheduler.earliest_deadline() == never);
    expect_clean_checks();
}

// Expiry against completion on host threads: CPU 1 expires a wait's deadline
// while CPU 0 completes the wait as ready. Exactly one of them completes it,
// the heap never keeps the key, and each round's thread is queued once.
void expiry_versus_completion()
{
    Fixture f;
    const auto pid = f.process();
    constexpr usize rounds = 20000;
    std::atomic<u64> armed{0}, started{0}, done{0};
    WaitTicket shared{};
    std::atomic<usize> timeouts{0}, readies{0};
    std::thread expirer([&] {
        const smp::ScopedHostedCpu cpu{1};
        for (u64 round = 1; round <= rounds; ++round)
        {
            while (armed.load(std::memory_order_acquire) != round) std::this_thread::yield();
            started.store(round, std::memory_order_release);
            for (usize spin = 0; spin < (round % 5) * 30; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
            std::array<u16, max_processes> due{};
            CHECK(f.scheduler.expire_deadlines(round * 10, due) == 0);
            done.store(round, std::memory_order_release);
        }
    });
    {
        const smp::ScopedHostedCpu cpu{0};
        for (u64 round = 1; round <= rounds; ++round)
        {
            f.run(pid);
            auto wait = f.scheduler.register_current_wait(ThreadWaitReason::deadline, round);
            CHECK(wait);
            shared = wait.value();
            CHECK(f.scheduler.arm_wait_deadline(shared, round * 10));
            f.scheduler.put_prev(0);
            armed.store(round, std::memory_order_release);
            // Odd rounds race the expiry as it starts; even ones complete at once.
            if (round % 2)
                while (started.load(std::memory_order_acquire) != round) std::this_thread::yield();
            for (usize spin = 0; spin < (round % 4) * 20; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
            const bool won = f.scheduler.complete_wait(shared, WaitCompletion::ready);
            while (done.load(std::memory_order_acquire) != round) std::this_thread::yield();
            const auto completion = f.thread(pid).wait_registration.completion();
            CHECK(!f.thread(pid).wait_registration.pending(shared));
            CHECK(won == (completion == WaitCompletion::ready));
            (won ? readies : timeouts).fetch_add(1, std::memory_order_relaxed);
            CHECK(f.scheduler.earliest_deadline() == never);
            CHECK(f.thread(pid).state == ProcessState::runnable && f.thread(pid).queued_cpu != no_cpu);
        }
    }
    expirer.join();
    CHECK(readies.load() + timeouts.load() == rounds && readies.load() > 0 && timeouts.load() > 0);
    expect_clean_checks();
    std::printf("expiry_vs_completion rounds=%zu ready=%zu timeout=%zu\n", rounds, readies.load(), timeouts.load());
}
} // namespace

int main()
{
    heap_against_model();
    heap_capacity();
    scheduler_wait_deadlines();
    scheduler_actions();
    scheduler_process_keys();
    expiry_versus_completion();
    std::printf("OK_DEADLINE_HEAP scope=model checks=%zu keys=%zu model_cpus=2 guest=0 hardware_smp=0\n",
                checks.load(), deadline_keys);
}
