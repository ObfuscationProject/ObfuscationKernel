// Wait channels (smp-scaling WP4b, spec 4.3) on the real spin locks
// (OK_HOSTED_LOCK_MODEL) with the lock-order checks on (OK_LOCK_DEBUG): the
// hash itself (FIFO wakes, batches, owner release, exhaustion), the
// scheduler's use of it (A1 links, A2 wakes, owner unlink, exhaustion
// retry, teardown), and host-thread races: the A3 insert-then-check of a
// poll against a waker, and wakes against owner releases. Host threads stand
// in for CPUs; no AP starts, no IPI is sent and no guest runs (scope=model).
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
#error "the wait hash races need the real spin locks"
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
constexpr auto pipe_read = static_cast<u8>(ThreadWaitReason::pipe_read);
constexpr auto never = DeadlineHeap<deadline_keys>::never;
using SmallHash = WaitHash<4, 8, smp::RankedSpinLock<smp::LockRank::wait_hash>>;

constexpr u16 none = 0xffff;
WaitHashTicket ticket(u64 thread, u64 generation = 1)
{
    return {static_cast<u32>(thread), static_cast<u32>(generation), static_cast<u16>(thread)};
}

void expect_clean_checks()
{
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu) CHECK(smp::lock_violation_count(cpu) == 0);
    CHECK(smp::check_total(smp::KernelCheck::queue) == 0);
    CHECK(smp::check_total(smp::KernelCheck::pin) == 0);
    CHECK(smp::check_total(smp::KernelCheck::audit_repairs) == 0);
}

// Two keys of one reason that share a bucket, and one that does not.
struct Keys
{
    u64 first{}, collides{}, apart{};
};
template <typename Hash> Keys colliding_keys()
{
    Keys keys{1, 0, 0};
    const auto bucket = Hash::bucket_of(pipe_read, keys.first);
    for (u64 key = 2; !keys.collides || !keys.apart; ++key)
    {
        if (!keys.collides && Hash::bucket_of(pipe_read, key) == bucket) keys.collides = key;
        if (!keys.apart && Hash::bucket_of(pipe_read, key) != bucket) keys.apart = key;
    }
    return keys;
}

// Wakes take the channel's entries oldest first and leave the other channels
// of the bucket alone. A taken entry stays its owner's until the owner
// releases it; release frees it once, linked or not, and keeps the owner's
// chain through `owner_next`.
void hash_basics()
{
    auto hash = std::make_unique<SmallHash>();
    const auto keys = colliding_keys<SmallHash>();
    const auto a = hash->insert(pipe_read, keys.first, ticket(1));
    const auto b = hash->insert(pipe_read, keys.collides, ticket(2), a);
    const auto c = hash->insert(pipe_read, keys.first, ticket(3), b);
    const auto d = hash->insert(pipe_read, keys.apart, ticket(4));
    CHECK(a != none && b != none && c != none && d != none && hash->claimed() == 4);
    CHECK(SmallHash::bucket_of(pipe_read, keys.first) == SmallHash::bucket_of(pipe_read, keys.collides));
    CHECK(SmallHash::bucket_of(pipe_read, keys.first) != SmallHash::bucket_of(pipe_read, keys.apart));
    CHECK(hash->linked(pipe_read, keys.first) == 2 && hash->linked(pipe_read, keys.collides) == 1);
    // Another reason with the same key is another channel.
    CHECK(hash->linked(static_cast<u8>(ThreadWaitReason::pipe_write), keys.first) == 0);
    std::array<WaitHashTicket, SmallHash::wake_batch> out{};
    auto taken = hash->take(pipe_read, keys.first, out);
    CHECK(taken.count == 2 && !taken.more && out[0] == ticket(1) && out[1] == ticket(3));
    CHECK(hash->linked(pipe_read, keys.first) == 0 && hash->linked(pipe_read, keys.collides) == 1);
    // Taken entries stay claimed until their owner releases them.
    CHECK(hash->claimed() == 4);
    // The owner's chain c -> b -> a: release walks it; a taken entry was
    // not linked any more, a pending one is detached.
    u16 next = none;
    CHECK(!hash->release(c, next) && next == b);
    CHECK(hash->release(b, next) && next == a);
    CHECK(!hash->release(a, next) && next == none);
    CHECK(hash->linked(pipe_read, keys.collides) == 0 && hash->claimed() == 1);
    // A released entry is free: a second release does nothing.
    CHECK(!hash->release(b, next) && next == none && hash->claimed() == 1);
    // A freed entry is reused.
    const auto reused = hash->insert(pipe_read, keys.collides, ticket(5));
    CHECK(reused != none && hash->claimed() == 2 && hash->linked(pipe_read, keys.collides) == 1);
    CHECK(hash->release(reused) && hash->release(d) && hash->claimed() == 0);
    // An empty channel takes nothing; a bad index releases nothing.
    taken = hash->take(pipe_read, keys.apart, out);
    CHECK(taken.count == 0 && !taken.more);
    CHECK(!hash->release(none) && !hash->release(100));
}

// The pool is fixed: an insert beyond it fails and counts, and a released
// entry serves the next insert.
void hash_exhaustion()
{
    auto hash = std::make_unique<SmallHash>();
    std::array<u16, SmallHash::capacity()> entries{};
    for (usize i = 0; i < entries.size(); ++i)
    {
        entries[i] = hash->insert(pipe_read, i, ticket(i + 1));
        CHECK(entries[i] != none);
    }
    CHECK(hash->claimed() == SmallHash::capacity() && hash->exhausted() == 0);
    CHECK(hash->insert(pipe_read, 99, ticket(9)) == none && hash->exhausted() == 1);
    // A taken entry still counts until it is released.
    std::array<WaitHashTicket, SmallHash::wake_batch> out{};
    CHECK(hash->take(pipe_read, 3, out).count == 1);
    CHECK(hash->insert(pipe_read, 99, ticket(9)) == none && hash->exhausted() == 2);
    CHECK(!hash->release(entries[3]));
    const auto again = hash->insert(pipe_read, 99, ticket(9));
    CHECK(again == entries[3]);
    for (usize i = 0; i < entries.size(); ++i) static_cast<void>(hash->release(entries[i]));
    CHECK(hash->claimed() == 0);
}

// A wake takes at most wake_batch entries per bucket pass and reports more;
// taking again drains the rest in order.
void hash_batches()
{
    using Hash = WaitHash<2, 64, smp::RankedSpinLock<smp::LockRank::wait_hash>>;
    auto hash = std::make_unique<Hash>();
    const auto keys = colliding_keys<Hash>();
    std::vector<u16> entries;
    for (u64 i = 1; i <= 40; ++i) entries.push_back(hash->insert(pipe_read, keys.first, ticket(i)));
    const auto other = hash->insert(pipe_read, keys.collides, ticket(99));
    std::array<WaitHashTicket, Hash::wake_batch> out{};
    u64 next = 1;
    usize passes = 0;
    for (;;)
    {
        const auto taken = hash->take(pipe_read, keys.first, out);
        ++passes;
        for (usize i = 0; i < taken.count; ++i) CHECK(out[i] == ticket(next++));
        CHECK(taken.more == (next <= 40));
        if (!taken.more) break;
    }
    CHECK(next == 41 && passes == 3);
    CHECK(hash->linked(pipe_read, keys.collides) == 1 && hash->claimed() == 41);
    for (const auto entry : entries) CHECK(!hash->release(entry));
    CHECK(hash->release(other) && hash->claimed() == 0);
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
        auto pid = scheduler.create_user_process("hash", context);
        CHECK(pid);
        thread(pid.value()).native_active = true;
        CHECK(scheduler.set_runnable(pid.value()));
        return pid.value();
    }
    ThreadControlBlock &thread(ProcessId pid) { return *scheduler.find_thread(pid, pid); }
    u16 index(ProcessId pid) { return scheduler.find(pid)->thread_index(thread(pid)); }
    usize linked(u64 key, ThreadWaitReason reason = ThreadWaitReason::pipe_read)
    {
        return scheduler.wait_channels().linked(static_cast<u8>(reason), key);
    }
    // CPU `cpu` runs `pid` (its leader is the only queued thread).
    void run(ProcessId pid, smp::CpuId cpu = 0)
    {
        const auto picked = scheduler.pick(cpu);
        CHECK(picked && picked->ref.pid == pid);
    }
};

// A1 arms link the wait on its own channel; A2 completes and unlinks it. A
// wait completed some other way (a timeout here) leaves its entry linked
// until its owner runs: settle_wait_links at the dispatch, or the owner's
// next arm, drops it, and a later wake cannot reach the new wait.
void scheduler_owner_unlink()
{
    Fixture f;
    const smp::ScopedHostedCpu cpu{0};
    const auto pid = f.process();
    f.run(pid);
    auto first = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, 7);
    CHECK(first && f.linked(7) == 1);
    f.scheduler.wake_waiters(ThreadWaitReason::pipe_read, 7);
    CHECK(f.linked(7) == 0 && f.thread(pid).wait_registration.completion() == WaitCompletion::ready);
    f.scheduler.put_prev(0);
    f.run(pid);
    // Not a channel reason: a poll or futex wait links nothing by itself.
    auto poll = f.scheduler.register_current_wait(ThreadWaitReason::poll, 0);
    CHECK(poll && f.scheduler.wait_channels().claimed() == 0);
    CHECK(f.scheduler.watch_channel(poll.value(), ThreadWaitReason::pipe_read, 8));
    CHECK(f.scheduler.watch_channel(poll.value(), ThreadWaitReason::pipe_write, 9));
    CHECK(f.linked(8) == 1 && f.linked(9, ThreadWaitReason::pipe_write) == 1);
    // Completed by its deadline, not by a wake: both entries stay linked.
    CHECK(f.scheduler.complete_wait(poll.value(), WaitCompletion::timeout));
    CHECK(f.linked(8) == 1 && f.linked(9, ThreadWaitReason::pipe_write) == 1);
    // Its owner's dispatch drops them (the thread still runs on CPU 0).
    f.scheduler.settle_wait_links(0);
    CHECK(f.linked(8) == 0 && f.linked(9, ThreadWaitReason::pipe_write) == 0);
    CHECK(f.scheduler.wait_channels().claimed() == 0);
    f.scheduler.put_prev(0);
    f.run(pid);
    // A pending wait keeps its entries across a dispatch.
    auto second = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, 7);
    CHECK(second && f.linked(7) == 1);
    f.scheduler.settle_wait_links(0);
    CHECK(f.linked(7) == 1);
    // Cancelled by its owner: the entry stays until the next arm drops it.
    CHECK(f.scheduler.complete_wait(second.value(), WaitCompletion::cancelled));
    f.scheduler.put_prev(0);
    f.run(pid);
    CHECK(f.linked(7) == 1);
    auto third = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, 11);
    CHECK(third && f.linked(7) == 0 && f.linked(11) == 1);
    // A wake of the old channel reaches nothing now.
    f.scheduler.wake_waiters(ThreadWaitReason::pipe_read, 7);
    CHECK(f.thread(pid).wait_registration.pending(third.value()));
    // A completed wait of the caller needs no entry: nothing is linked.
    CHECK(f.scheduler.watch_channel(first.value(), ThreadWaitReason::pipe_read, 12) && f.linked(12) == 0);
    // Watching for a ticket other than the caller's own is refused.
    auto foreign = third.value();
    ++foreign.thread;
    CHECK(!f.scheduler.watch_channel(foreign, ThreadWaitReason::pipe_read, 12) && f.linked(12) == 0);
    // Exit drops the entries and the deadline of every thread.
    CHECK(f.scheduler.arm_wait_deadline(third.value(), 500));
    f.scheduler.put_prev(0);
    CHECK(f.scheduler.exit_process(pid, 0));
    CHECK(f.linked(11) == 0 && f.scheduler.wait_channels().claimed() == 0);
    CHECK(f.scheduler.earliest_deadline() == never);
    expect_clean_checks();
}

// Exhaustion: with no entry left an arm still succeeds; the wait counts as
// unqueued and retries on a deadline a tick out, whose expiry completes it
// ready so that its owner re-evaluates. A wake of its channel cannot reach it.
void scheduler_exhaustion()
{
    Fixture f;
    const smp::ScopedHostedCpu cpu{0};
    const auto pid = f.process();
    auto &channels = f.scheduler.wait_channels();
    std::vector<u16> fillers;
    for (usize i = 0; i < wait_hash_entries; ++i)
    {
        const auto entry = channels.insert(static_cast<u8>(ThreadWaitReason::child), 1000 + i, ticket(400));
        CHECK(entry != none);
        fillers.push_back(entry);
    }
    CHECK(channels.claimed() == wait_hash_entries);
    const auto unqueued = smp::evidence_total(&smp::EvidenceCpu::unqueued);
    f.run(pid);
    auto wait = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, 21);
    CHECK(wait && f.linked(21) == 0);
    CHECK(f.scheduler.unqueued_waits() == 1 && smp::evidence_total(&smp::EvidenceCpu::unqueued) == unqueued + 1);
    const auto retry = f.scheduler.deadline_of(f.index(pid));
    CHECK(retry != never && f.scheduler.earliest_deadline() == retry);
    f.scheduler.wake_waiters(ThreadWaitReason::pipe_read, 21);
    CHECK(f.thread(pid).wait_registration.pending(wait.value()));
    f.scheduler.put_prev(0);
    std::array<u16, max_processes> due{};
    CHECK(f.scheduler.expire_deadlines(retry - 1, due) == 0 && f.thread(pid).wait_registration.pending(wait.value()));
    CHECK(f.scheduler.expire_deadlines(retry, due) == 0);
    CHECK(f.thread(pid).wait_registration.completion() == WaitCompletion::ready);
    CHECK(f.thread(pid).state == ProcessState::runnable && f.thread(pid).queued_cpu != no_cpu);
    CHECK(f.scheduler.earliest_deadline() == never);
    for (const auto entry : fillers) CHECK(channels.release(entry));
    CHECK(channels.claimed() == 0);
    // Entries are back: the next arm links again.
    f.run(pid);
    auto linked = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, 21);
    CHECK(linked && f.linked(21) == 1 && f.scheduler.unqueued_waits() == 1);
    f.scheduler.wake_waiters(ThreadWaitReason::pipe_read, 21);
    CHECK(!f.thread(pid).wait_registration.pending(linked.value()));
    f.scheduler.put_prev(0);
    expect_clean_checks();
}

// The poll part of the audit (spec 4.10): a parked poll waiter is probed and
// queued, so that its owner's dispatch re-evaluates readiness; it is picked
// blocked and, its wait still pending, parks again unqueued. Other parked
// waits and a probed or queued poll are left alone.
void scheduler_poll_audit()
{
    Fixture f;
    const smp::ScopedHostedCpu cpu{0};
    const auto pid = f.process();
    const auto other = f.process();
    f.run(pid);
    auto poll = f.scheduler.register_current_wait(ThreadWaitReason::poll, 0);
    CHECK(poll && f.scheduler.watch_channel(poll.value(), ThreadWaitReason::pipe_read, 31));
    f.scheduler.put_prev(0);
    f.run(other);
    auto pipe = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, 32);
    CHECK(pipe);
    f.scheduler.put_prev(0);
    CHECK(f.thread(pid).queued_cpu == no_cpu && f.thread(other).queued_cpu == no_cpu);
    CHECK(f.scheduler.probe_poll_waiters() == 1);
    CHECK(f.thread(pid).probe && f.thread(pid).queued_cpu != no_cpu && f.thread(other).queued_cpu == no_cpu);
    CHECK(f.scheduler.probe_poll_waiters() == 0); // Already probed and queued.
    const auto picked = f.scheduler.pick(0);
    CHECK(picked && picked->ref.pid == pid && picked->probed && !f.thread(pid).probe);
    CHECK(f.thread(pid).wait_registration.pending(poll.value()) && !f.scheduler.resume_current(0));
    // Its entry stays linked while the wait is pending.
    f.scheduler.settle_wait_links(0);
    CHECK(f.linked(31) == 1);
    f.scheduler.put_prev(0);
    CHECK(f.thread(pid).state == ProcessState::blocked && f.thread(pid).queued_cpu == no_cpu);
    // A wake of its channel still completes it.
    f.scheduler.wake_waiters(ThreadWaitReason::pipe_read, 31);
    CHECK(!f.thread(pid).wait_registration.pending(poll.value()) && f.thread(pid).queued_cpu != no_cpu);
    f.scheduler.wake_waiters(ThreadWaitReason::pipe_read, 32);
    expect_clean_checks();
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

// A3 (spec 4.3): a poller arms its wait, links its channel and only then
// checks the condition under the object's lock; a waker sets the condition
// under that lock and wakes the channel while still holding it. Whichever
// order they run in, the poll ends complete: either its check saw the
// condition (it completes itself) or the waker found its entry. A wait still
// pending after both finished is a lost wakeup. Rounds continue past the
// usual budget until both outcomes occurred (race_coverage.hpp).
void a3_insert_then_check()
{
    Fixture f;
    const auto pid = f.process();
    test::CoverageRounds rounds{20000, 200000};
    constexpr u64 key = 77;
    // A round's plan, published by the poller before the round's first gate.
    enum : u8
    {
        run = 1,
        link_first = 2,  // The waker waits for the poller's link.
        poller_late = 4, // The poller is nudged before it arms.
        waker_late = 8,  // The waker is nudged before it sets the condition.
    };
    std::atomic<u8> plan{0};
    smp::RankedSpinLock<smp::LockRank::pipe> object_lock{};
    bool condition = false; // Guarded by object_lock.
    Barrier barrier{2};
    std::atomic<u64> linked{0};
    std::atomic<usize> checked{0}, woken{0}, lost{0};
    std::thread waker([&] {
        const smp::ScopedHostedCpu cpu{1};
        for (usize round = 1;; ++round)
        {
            barrier.arrive_and_wait();
            const auto step = plan.load(std::memory_order_acquire);
            if (!(step & run)) break;
            if (step & link_first)
                while (linked.load(std::memory_order_acquire) != round) std::this_thread::yield();
            for (usize spin = 0; spin < (round % 7) * 30; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
            if (step & waker_late) test::nudge();
            object_lock.lock();
            condition = true;
            f.scheduler.wake_waiters(ThreadWaitReason::pipe_read, key);
            object_lock.unlock();
            barrier.arrive_and_wait();
        }
    });
    {
        const smp::ScopedHostedCpu cpu{0};
        for (usize round = 1;; ++round)
        {
            const bool more = rounds.next(checked.load() > 0 && woken.load() > 0);
            // Odd rounds let the poller link first; even ones race it. A
            // nudged extension round delays the poller of a racing round
            // while no poll saw the condition (the waker can then finish
            // before the link), else the waker after the link.
            u8 step = more ? run : 0;
            if (rounds.nudged()) step |= checked.load() == 0 ? poller_late : waker_late | link_first;
            else if (round % 2) step |= link_first;
            plan.store(step, std::memory_order_release);
            barrier.arrive_and_wait();
            if (!more) break;
            if (step & poller_late) test::nudge();
            f.run(pid);
            auto wait = f.scheduler.register_current_wait(ThreadWaitReason::poll, 0);
            CHECK(wait);
            // A wake that took the entry before the link was recorded leaves
            // the wait complete and nothing to watch: still true.
            CHECK(f.scheduler.watch_channel(wait.value(), ThreadWaitReason::pipe_read, key));
            linked.store(round, std::memory_order_release);
            for (usize spin = 0; spin < (round % 5) * 40; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
            object_lock.lock();
            const bool seen = condition;
            object_lock.unlock();
            // Ready at the check: the owner completes its own ticket (a waker
            // that got there first leaves it completed already).
            if (seen && f.scheduler.complete_wait(wait.value(), WaitCompletion::ready))
                checked.fetch_add(1, std::memory_order_relaxed);
            f.scheduler.put_prev(0);
            barrier.arrive_and_wait();
            const auto &thread = f.thread(pid);
            if (thread.wait_registration.pending(wait.value())) lost.fetch_add(1, std::memory_order_relaxed);
            // Not seen at the check: only the waker's take can have completed it.
            else if (!seen) woken.fetch_add(1, std::memory_order_relaxed);
            CHECK(!thread.wait_registration.pending(wait.value()));
            // The next round starts from a clean channel and a queued thread.
            object_lock.lock();
            condition = false;
            object_lock.unlock();
            f.run(pid);
            f.scheduler.settle_wait_links(0);
            CHECK(f.linked(key) == 0);
            f.scheduler.put_prev(0);
        }
    }
    waker.join();
    std::printf("a3_insert_then_check rounds=%zu min_rounds=%zu self_completed=%zu woken=%zu lost=%zu\n",
                rounds.rounds(), rounds.minimum(), checked.load(), woken.load(), lost.load());
    std::fflush(stdout);
    CHECK(lost.load() == 0);
    CHECK(f.scheduler.wait_channels().claimed() == 0);
    expect_clean_checks();
    // Both interleavings happened: some polls saw the condition at their
    // check, others slept and were completed through their entry.
    CHECK(checked.load() > 0 && woken.load() > 0);
}

// Wakes against owner releases on one channel: four CPUs link and release
// their own entries while two wake the channel. Every entry is detached once
// (by a take or by its release), freed once by its owner (the pool ends
// empty), and no lock order is broken.
void wake_versus_release()
{
    auto hash = std::make_unique<WaitChannels>();
    std::atomic<bool> stop{false};
    std::atomic<usize> owner_unlinks{0}, taken{0};
    std::vector<std::thread> threads;
    for (smp::CpuId cpu = 0; cpu < cpus; ++cpu)
        threads.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu hosted{cpu};
            u64 generation = 1;
            while (!stop.load(std::memory_order_acquire))
            {
                const auto entry = hash->insert(pipe_read, 5, ticket(cpu + 1, generation++));
                CHECK(entry != none);
                for (int spin = 0; spin < 16; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
                if (hash->release(entry)) owner_unlinks.fetch_add(1, std::memory_order_relaxed);
            }
        });
    for (smp::CpuId cpu = cpus; cpu < cpus + 2; ++cpu)
        threads.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu hosted{cpu};
            std::array<WaitHashTicket, WaitChannels::wake_batch> out{};
            while (!stop.load(std::memory_order_acquire))
                taken.fetch_add(hash->take(pipe_read, 5, out).count, std::memory_order_relaxed);
        });
    // Until both detach paths occurred (race_coverage.hpp).
    const auto ran = test::run_until_covered(std::chrono::milliseconds(1500), std::chrono::seconds(30),
                                             [&] { return owner_unlinks.load() > 0 && taken.load() > 0; });
    stop.store(true, std::memory_order_release);
    for (auto &thread : threads) thread.join();
    std::printf("wake_vs_release ms=%zu owner_unlinks=%zu taken=%zu\n", ran, owner_unlinks.load(), taken.load());
    std::fflush(stdout);
    CHECK(hash->claimed() == 0 && hash->linked(pipe_read, 5) == 0);
    CHECK(owner_unlinks.load() > 0 && taken.load() > 0);
    expect_clean_checks();
}
} // namespace

int main()
{
    hash_basics();
    hash_exhaustion();
    hash_batches();
    scheduler_owner_unlink();
    scheduler_exhaustion();
    scheduler_poll_audit();
    a3_insert_then_check();
    wake_versus_release();
    std::printf("OK_WAIT_HASH scope=model checks=%zu buckets=%zu entries=%zu model_cpus=%zu guest=0 hardware_smp=0\n",
                checks.load(), wait_hash_buckets, wait_hash_entries, cpus + 2);
}
