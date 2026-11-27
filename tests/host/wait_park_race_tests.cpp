// Host-thread races of the queue path (spec 4.2) on the real task and run
// queue locks (OK_HOSTED_LOCK_MODEL): park against complete, steal against
// re-enqueue, and the idle handshake against a push. Host threads stand in
// for CPUs; no AP starts, no IPI is sent and no guest runs (scope=model).
#include "ok/sched/scheduler.hpp"
#include "race_coverage.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#if !defined(OK_HOSTED_LOCK_MODEL)
#error "the park races need the real spin locks"
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
    // A user process whose leader is native and queued.
    ProcessId process()
    {
        auto pid = scheduler.create_user_process("race", context);
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
    // Entries naming this thread across every queue.
    usize entries(ProcessId pid, ThreadId tid) const
    {
        usize count = 0;
        for (smp::CpuId cpu = 0; cpu < cpus; ++cpu) count += scheduler.queued_on(cpu, {pid, tid});
        return count;
    }
    // Quiescent: every thread of `pid` is runnable, off its CPU and held by
    // exactly the one entry its queued_cpu names; nothing was lost or doubled.
    void expect_queued_once(ProcessId pid)
    {
        for (const auto &thread : scheduler.find(pid)->live_threads())
        {
            CHECK(thread.state == ProcessState::runnable && thread.on_cpu == no_cpu);
            CHECK(thread.queued_cpu < cpus && entries(pid, thread.tid) == 1);
            CHECK(scheduler.queued_on(thread.queued_cpu, {pid, thread.tid}));
        }
    }
};

void expect_clean_checks()
{
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu) CHECK(smp::lock_violation_count(cpu) == 0);
    CHECK(smp::check_total(smp::KernelCheck::queue) == 0);
    CHECK(smp::check_total(smp::KernelCheck::pin) == 0);
    CHECK(smp::check_total(smp::KernelCheck::audit_repairs) == 0);
}

// One gate per round: every party arrives, then all leave together.
class Barrier
{
  public:
    explicit Barrier(usize parties) : parties_(parties) {}
    void arrive_and_wait()
    {
        const auto generation = generation_.load(std::memory_order_acquire);
        if (arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == parties_)
        {
            arrived_.store(0, std::memory_order_relaxed);
            generation_.store(generation + 1, std::memory_order_release);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation) std::this_thread::yield();
    }

  private:
    usize parties_;
    std::atomic<usize> arrived_{0};
    std::atomic<usize> generation_{0};
};

// Park against complete: CPU 0 arms a wait and parks the thread (put_prev)
// while CPU 1 completes the ticket. Whichever runs first, the thread ends
// runnable and queued exactly once (spec 4.2 lost-wakeup proof). Rounds
// continue past the usual budget until both orders occurred
// (race_coverage.hpp).
void park_versus_complete()
{
    Fixture f;
    const auto pid = f.process();
    test::CoverageRounds rounds{20000, 200000};
    // A round's plan, published by CPU 0 before the round's first gate.
    enum : u8
    {
        run = 1,
        parker_late = 2, // CPU 0 is nudged before it parks.
        waker_late = 4,  // CPU 1 is nudged before it completes.
    };
    std::atomic<u8> plan{0};
    Barrier barrier{2};
    std::atomic<u64> published{0};
    WaitTicket shared{};
    std::atomic<usize> before_park{0}, after_park{0};
    // The waker reads the slot's on_cpu mirror (an atomic) to tell which
    // side of the park its completion landed on.
    const auto pinned = f.scheduler.pin(pid);
    CHECK(pinned);
    std::thread waker([&] {
        const smp::ScopedHostedCpu cpu{1};
        for (usize round = 1;; ++round)
        {
            barrier.arrive_and_wait();
            const auto step = plan.load(std::memory_order_acquire);
            if (!(step & run)) break;
            while (published.load(std::memory_order_acquire) != round) std::this_thread::yield();
            for (usize spin = 0; spin < (round % 5) * 40; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
            if (step & waker_late) test::nudge();
            const bool parked = pinned.slot()->on_cpu[0].load(std::memory_order_acquire) == no_cpu;
            CHECK(f.scheduler.complete_wait(shared, WaitCompletion::ready));
            (parked ? after_park : before_park).fetch_add(1, std::memory_order_relaxed);
            barrier.arrive_and_wait();
        }
    });
    {
        const smp::ScopedHostedCpu cpu{0};
        for (usize round = 1;; ++round)
        {
            const bool more = rounds.next(before_park.load() > 0 && after_park.load() > 0);
            // A nudged extension round delays the park while no completion
            // landed before one, else the completion.
            u8 step = more ? run : 0;
            if (rounds.nudged()) step |= before_park.load() == 0 ? parker_late : waker_late;
            plan.store(step, std::memory_order_release);
            barrier.arrive_and_wait();
            if (!more) break;
            const auto picked = f.scheduler.pick(0);
            CHECK(picked && picked->ref == (ThreadRef{pid, pid}) && !picked->probed);
            auto ticket = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, round);
            CHECK(ticket);
            shared = ticket.value();
            published.store(round, std::memory_order_release);
            // The park races the completion; odd rounds give the waker a head start.
            if (round % 2)
                for (usize spin = 0; spin < 400; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
            if (step & parker_late) test::nudge();
            f.scheduler.put_prev(0);
            barrier.arrive_and_wait();
            const auto &thread = f.thread(pid);
            CHECK(thread.wait_registration.completion() == WaitCompletion::ready);
            f.expect_queued_once(pid);
        }
    }
    waker.join();
    std::printf("park_vs_complete rounds=%zu min_rounds=%zu completed_armed=%zu completed_parked=%zu\n",
                rounds.rounds(), rounds.minimum(), before_park.load(), after_park.load());
    std::fflush(stdout);
    CHECK(f.scheduler.wakes(WakeSource::thread) == rounds.rounds());
    expect_clean_checks();
    // Both interleavings must have been exercised.
    CHECK(before_park.load() > 0 && after_park.load() > 0);
}

// Steal against re-enqueue: four CPUs pick, run briefly and put back three
// threads of two processes. Every pick races other CPUs' own pops, steals
// and put_prev pushes. No thread ever runs on two CPUs, and once all stop
// each is queued exactly once.
void steal_versus_requeue()
{
    Fixture f;
    const auto first = f.process();
    const auto sibling = f.add_thread(first);
    const auto second = f.process();
    const ThreadRef threads[] = {{first, first}, {first, sibling}, {second, second}};
    std::atomic<u32> running[3]{};
    std::atomic<usize> picks{0}, steals{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (smp::CpuId cpu = 0; cpu < cpus; ++cpu)
        workers.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu hosted{cpu};
            while (!stop.load(std::memory_order_acquire))
            {
                const auto picked = f.scheduler.pick(cpu);
                if (!picked) continue;
                usize index = 3;
                for (usize i = 0; i < 3; ++i)
                    if (threads[i] == picked->ref) index = i;
                CHECK(index < 3 && !picked->probed);
                CHECK(running[index].exchange(1, std::memory_order_acq_rel) == 0);
                CHECK(f.scheduler.current_tid(cpu) == picked->ref.tid);
                for (int spin = 0; spin < 32; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
                running[index].store(0, std::memory_order_release);
                f.scheduler.put_prev(cpu);
                picks.fetch_add(1, std::memory_order_relaxed);
            }
        });
    // Until enough picks raced (race_coverage.hpp).
    const auto ran = test::run_until_covered(std::chrono::milliseconds(1500), std::chrono::seconds(30),
                                             [&] { return picks.load() > 1000; });
    stop.store(true, std::memory_order_release);
    for (auto &worker : workers) worker.join();
    std::printf("steal_vs_requeue ms=%zu picks=%zu\n", ran, picks.load());
    std::fflush(stdout);
    for (const auto &thread : threads) CHECK(f.entries(thread.pid, thread.tid) == 1);
    f.expect_queued_once(first);
    f.expect_queued_once(second);
    for (smp::CpuId cpu = 0; cpu < cpus; ++cpu)
    {
        CHECK(!f.scheduler.cpu_slot(cpu).occupied());
        steals.fetch_add(smp::evidence_cpus[cpu].steals.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    CHECK(picks.load() > 1000);
    expect_clean_checks();
    std::printf("steal_vs_requeue steals=%zu\n", steals.load());
}

// The idle handshake against a push: CPU 1 runs a thread confined to it,
// arms its wait and parks it, then goes idle; CPU 0 completes the wait,
// which pushes the thread to CPU 1's queue. CPU 1 halts only when enter_idle
// saw every queue empty, and then waits for the wake request a waker leaves
// when it finds CPU 1 idle after its push. A lost wake leaves the entry
// queued and CPU 1 halted: the round times out. Rounds continue past the
// usual budget until CPU 1 halted at least once (race_coverage.hpp).
void idle_versus_push()
{
    Fixture f;
    const auto pid = f.process();
    CHECK(f.scheduler.set_thread_affinity(*f.scheduler.find(pid), f.thread(pid), 1u << 1));
    // Move the entry onto CPU 1's queue: CPU 1 picks it and parks it runnable.
    {
        const smp::ScopedHostedCpu cpu{1};
        for (int attempt = 0; attempt < 4 && !f.scheduler.pick(1); ++attempt) {}
        CHECK(f.scheduler.current_tid(1) == pid);
        f.scheduler.put_prev(1);
    }
    // Owned by CPU 1, which decides before each arm whether another round runs.
    test::CoverageRounds rounds{20000, 200000};
    std::atomic<u64> armed{0}, served{0}, nudged{0};
    WaitTicket shared{};
    std::atomic<usize> halts{0}, woken{0}, retries{0};
    std::atomic<bool> lost{false}, finished{false};
    std::thread idle_cpu([&] {
        const smp::ScopedHostedCpu cpu{1};
        usize round = 0;
        while (!lost.load(std::memory_order_relaxed))
        {
            if (round == rounds.rounds())
            {
                if (!rounds.next(halts.load() > 0)) break;
                // A nudged extension round delays the completion, so CPU 1
                // reaches its idle path first.
                if (rounds.nudged()) nudged.store(rounds.rounds(), std::memory_order_relaxed);
            }
            if (const auto picked = f.scheduler.pick(1))
            {
                CHECK(picked->ref == (ThreadRef{pid, pid}));
                f.scheduler.leave_idle(1);
                ++round;
                auto ticket = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, round);
                CHECK(ticket);
                shared = ticket.value();
                f.scheduler.put_prev(1);
                armed.store(round, std::memory_order_release);
                continue;
            }
            const auto check = f.scheduler.enter_idle(1);
            if (check != SchedulerState::IdleCheck::halt)
            {
                retries.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Halted: only a wake request ends the halt.
            halts.fetch_add(1, std::memory_order_relaxed);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!f.scheduler.take_wake_request(1))
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    // Not woken: lost only if work is waiting on this CPU.
                    if (f.scheduler.run_queue(1).size() != 0) lost.store(true, std::memory_order_relaxed);
                    break;
                }
                if (served.load(std::memory_order_acquire) == rounds.rounds()) break;
                std::this_thread::yield();
            }
            woken.fetch_add(1, std::memory_order_relaxed);
        }
        finished.store(true, std::memory_order_release);
    });
    {
        const smp::ScopedHostedCpu cpu{0};
        for (usize round = 1; !lost.load(std::memory_order_relaxed); ++round)
        {
            while (armed.load(std::memory_order_acquire) != round && !lost.load(std::memory_order_relaxed) &&
                   !finished.load(std::memory_order_acquire))
                std::this_thread::yield();
            // CPU 1 stops only after its last arm, so every armed round is served.
            if (armed.load(std::memory_order_acquire) != round) break;
            // Let CPU 1 reach its idle path on some rounds.
            for (usize spin = 0; spin < (round % 7) * 50; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
            if (nudged.load(std::memory_order_relaxed) == round) test::nudge();
            CHECK(f.scheduler.complete_wait(shared, WaitCompletion::ready));
            served.store(round, std::memory_order_release);
        }
    }
    idle_cpu.join();
    std::printf("idle_vs_push rounds=%zu min_rounds=%zu halts=%zu retries=%zu lost=%d\n", rounds.rounds(),
                rounds.minimum(), halts.load(), retries.load(), lost.load() ? 1 : 0);
    std::fflush(stdout);
    CHECK(!lost.load());
    CHECK(f.scheduler.wakes(WakeSource::thread) == rounds.rounds());
    CHECK(halts.load() > 0);
    expect_clean_checks();
}
} // namespace

int main()
{
    park_versus_complete();
    steal_versus_requeue();
    idle_versus_push();
    std::printf("OK_WAIT_PARK_RACE scope=model checks=%zu model_cpus=%zu host_threads=%zu guest=0 hardware_smp=0\n",
                checks.load(), cpus, cpus);
}
