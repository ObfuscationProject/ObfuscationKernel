// Futex hash buckets (smp-scaling WP4c, spec 4.5) on the real spin locks
// (OK_HOSTED_LOCK_MODEL) with the lock-order checks on (OK_LOCK_DEBUG).
//
// The actual runtime and scheduler with hosted user copies: a timed-out
// waiter against WAKE(1), signal against wake, requeue across buckets and an
// equal-bucket pair for WAKE_OP and CMP_REQUEUE, with no lock-order report.
// Then host-thread races on the bucket list itself (FutexBuckets with real
// futex_bucket locks): compare-and-queue against a waker that changes the
// word, a requeue moving a wait while its owner unlinks it, and a signal's
// interrupt against WAKE(1). Host threads stand in for CPUs; no AP starts,
// no IPI is sent and no guest runs (scope=model).
// The host thread headers come first: the hosted model's `using namespace
// ok` would make libstdc++'s syscall() ambiguous.
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "native_clock_model.hpp"
#include "ok/abi/linux_futex_wake_operation.hpp"

#if !defined(OK_HOSTED_LOCK_MODEL) || !defined(OK_LOCK_DEBUG)
#error "the futex bucket model needs the real spin locks and their order checks"
#endif

namespace
{
using Domain = native::FutexKeyDomain;
constexpr uptr base = hw::user_begin + 128;
constexpr auto none = native::futex_no_bucket;
usize rounds_checked{};

struct Owner
{
    sched::ProcessId pid{};
    std::array<sched::ThreadId, sched::max_threads_per_process> tids{};
};
Owner owner(Kernel &kernel, usize count)
{
    auto made = kernel.scheduler().create_user_process(
        "futex-bucket-model", kernel.arch().make_user_context({hw::user_begin, hw::user_end - hw::page_size}));
    CHECK(made);
    Owner result{made.value(), {}};
    auto *process = kernel.scheduler().find(result.pid);
    result.tids[0] = process->leader().tid;
    for (usize i = 1; i < count; ++i)
    {
        auto created = kernel.scheduler().create_thread(result.pid, process->leader().context);
        CHECK(created);
        result.tids[i] = created.value();
    }
    CHECK(kernel.scheduler().set_runnable(result.pid));
    for (auto &thread : process->thread_records()) thread.native_active = true;
    return result;
}
void retire(Kernel &kernel, Owner group)
{
    native::release_process(kernel, group.pid);
    CHECK(kernel.scheduler().exit_process(group.pid, 0));
    CHECK(!native::futex_objects.accounting().live_objects);
}
void activate(Kernel &kernel, Owner group, usize index = 0)
{
    CHECK(kernel.scheduler().activate_thread(group.pid, group.tids[index]));
}
void put(uptr address, u32 value)
{
    for (usize i = 0; i < 4; ++i) mock::memory[address - hw::user_begin + i] = static_cast<std::byte>(value >> (8 * i));
}
u32 get(uptr address)
{
    u32 value{};
    for (usize i = 0; i < 4; ++i) value |= static_cast<u32>(mock::memory[address - hw::user_begin + i]) << (8 * i);
    return value;
}
u32 bucket_of(Owner group, uptr address)
{
    native::FutexWait::Key key{};
    key.domain = Domain::private_address_space;
    key.pid = group.pid;
    key.address = address;
    return native::futex_bucket_of(key);
}
native::FutexWait &record(memory::ObjectHandle object)
{
    auto resolved = native::futex_objects.resolve(object);
    CHECK(resolved);
    return *resolved.value();
}
memory::ObjectHandle wait(Kernel &kernel, Owner group, usize index, uptr address, bool timed = false)
{
    activate(kernel, group, index);
    kernel.scheduler().current_thread()->syscall_pending = true;
    bool complete = true;
    CHECK(native::futex(kernel, group.pid, {native::FutexOperation::wait, address, get(address), {0, 17}, timed},
                        complete) == 0 && !complete);
    const auto object = native::futex_object(group.pid, group.tids[index]);
    CHECK(object.pool && record(object).bucket == bucket_of(group, address));
    return object;
}
i64 wake(Kernel &kernel, Owner group, uptr address, u32 count)
{
    activate(kernel, group);
    bool complete = true;
    const auto result = native::futex(kernel, group.pid, {native::FutexOperation::wake, address, count}, complete);
    CHECK(complete);
    return result;
}
// The owner's resume as the step runs it once the waiter is picked.
std::optional<i64> resume(Kernel &kernel, Owner group, usize index)
{
    activate(kernel, group, index);
    auto *process = kernel.scheduler().find(group.pid);
    auto *thread = kernel.scheduler().current_thread();
    bool complete = true;
    const auto result = native::resume_owned_wait(kernel, *process, *thread, complete);
    if (result)
    {
        thread->syscall_pending = false;
        CHECK(native::set_result(*process, thread->native_frame, *result));
    }
    return result;
}
void expire(Kernel &kernel, u64 now)
{
    std::array<u16, sched::max_processes> due{};
    static_cast<void>(kernel.scheduler().expire_deadlines(now, due));
}
sched::WaitCompletion completion(Kernel &kernel, Owner group, usize index)
{
    const auto *thread = kernel.scheduler().find_thread(group.pid, group.tids[index]);
    CHECK(thread);
    return thread->wait_registration.active() ? sched::WaitCompletion::none : thread->wait_registration.completion();
}
// Two word addresses of one process: in different buckets, or in the same.
std::pair<uptr, uptr> addresses(Owner group, bool same_bucket)
{
    for (uptr second = base + 4; second < hw::user_begin + mock::memory.size() - 4; second += 4)
        if ((bucket_of(group, base) == bucket_of(group, second)) == same_bucket) return {base, second};
    CHECK(false);
    return {};
}
void expect_clean_locks()
{
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
    {
        CHECK(smp::lock_violation_count(cpu) == 0);
        CHECK(smp::lock_debug_max_depth(cpu) <= smp::lock_debug_capacity);
    }
    CHECK(smp::check_total(smp::KernelCheck::class_rule) == 0);
    CHECK(smp::check_total(smp::KernelCheck::held) == 0);
    CHECK(smp::check_total(smp::KernelCheck::queue) == 0);
}

// A waiter whose wait timed out but has not resumed is still queued: WAKE(1)
// unlinks it uncounted and wakes the next waiter.
void timed_out_against_wake(Kernel &kernel)
{
    const auto group = owner(kernel, 4);
    put(base, 7);
    const auto timed = wait(kernel, group, 1, base, true);
    const auto plain = wait(kernel, group, 2, base);
    expire(kernel, mock::now + 17);
    CHECK(completion(kernel, group, 1) == sched::WaitCompletion::timeout);
    CHECK(record(timed).bucket != none);
    CHECK(wake(kernel, group, base, 1) == 1);
    CHECK(completion(kernel, group, 2) == sched::WaitCompletion::ready);
    CHECK(record(timed).bucket == none && record(plain).bucket == none);
    CHECK(resume(kernel, group, 1) == i64{-110} && resume(kernel, group, 2) == i64{0});
    CHECK(!native::futex_objects.resolve(timed) && !native::futex_objects.resolve(plain));
    // The same with the timed-out waiter resumed first: its resume unlinks
    // it, and WAKE(1) still wakes the other.
    const auto again = wait(kernel, group, 1, base, true);
    const auto other = wait(kernel, group, 3, base);
    expire(kernel, mock::now + 17);
    CHECK(resume(kernel, group, 1) == i64{-110} && !native::futex_objects.resolve(again));
    CHECK(native::futex_buckets[bucket_of(group, base)].head == &record(other));
    CHECK(wake(kernel, group, base, 1) == 1 && resume(kernel, group, 3) == i64{0});
    retire(kernel, group);
}

// Signal against wake, through the signal path's bucket step (the hosted
// x86_64 signal suites run the full delivery).
void signal_against_wake(Kernel &kernel)
{
    const auto group = owner(kernel, 4);
    put(base, 7);
    usize published = 0;
    const auto publish = [&published] { ++published; return true; };
    // The wake first: the interrupt finds the wait completed and publishes
    // nothing; the resume produces 0.
    auto a = wait(kernel, group, 1, base);
    CHECK(wake(kernel, group, base, 1) == 1);
    CHECK(native::interrupt_futex_wait(kernel, record(a), publish) == native::FutexInterrupt::completed);
    CHECK(published == 0 && resume(kernel, group, 1) == i64{0});
    // The signal first: the wait leaves its queue interrupted, so WAKE(1)
    // wakes the next waiter instead.
    a = wait(kernel, group, 1, base);
    const auto b = wait(kernel, group, 2, base);
    CHECK(native::interrupt_futex_wait(kernel, record(a), publish) == native::FutexInterrupt::interrupted);
    CHECK(published == 1 && completion(kernel, group, 1) == sched::WaitCompletion::interrupted);
    CHECK(record(a).bucket == none);
    CHECK(wake(kernel, group, base, 1) == 1 && completion(kernel, group, 2) == sched::WaitCompletion::ready);
    CHECK(native::cancel_futex(kernel, a, "interrupted", -4));
    CHECK(resume(kernel, group, 2) == i64{0} && !native::futex_objects.resolve(b));
    // The timeout first: the interrupt publishes nothing; the resume, -110.
    const auto timed = wait(kernel, group, 1, base, true);
    expire(kernel, mock::now + 17);
    CHECK(native::interrupt_futex_wait(kernel, record(timed), publish) == native::FutexInterrupt::completed);
    CHECK(published == 1 && record(timed).bucket == none);
    CHECK(resume(kernel, group, 1) == i64{-110});
    // A failed publication leaves the wait pending and reports it.
    const auto lost = wait(kernel, group, 1, base);
    CHECK(native::interrupt_futex_wait(kernel, record(lost), [] { return false; }) == native::FutexInterrupt::failed);
    CHECK(completion(kernel, group, 1) == sched::WaitCompletion::none);
    CHECK(native::cancel_futex(kernel, lost));
    retire(kernel, group);
}

// REQUEUE between two buckets: the moved waits join the destination queue
// last, numbered by its sequence, and their owners unlink them there.
void requeue_across_buckets(Kernel &kernel)
{
    const auto group = owner(kernel, 6);
    const auto [source, destination] = addresses(group, false);
    const auto from = bucket_of(group, source), to = bucket_of(group, destination);
    put(source, 7); put(destination, 9);
    const auto a = wait(kernel, group, 1, source), b = wait(kernel, group, 2, source);
    const auto c = wait(kernel, group, 3, destination), d = wait(kernel, group, 4, source);
    const auto before = native::futex_buckets[to].sequence;
    activate(kernel, group);
    CHECK(native::futex_requeue(kernel, group.pid, {source, destination, 1, 2, 7, true, false,
                                                    Domain::private_address_space}) == 3);
    // A woke; B and D moved behind C.
    CHECK(completion(kernel, group, 1) == sched::WaitCompletion::ready && record(a).bucket == none);
    CHECK(native::futex_buckets[from].head == nullptr && native::futex_buckets[from].tail == nullptr);
    const auto &queue = native::futex_buckets[to];
    CHECK(queue.head == &record(c) && record(c).bucket_next == &record(b) && record(b).bucket_next == &record(d) &&
          queue.tail == &record(d));
    CHECK(record(b).bucket == to && record(d).bucket == to && record(b).key.address == destination);
    CHECK(record(b).queue_sequence == before + 1 && record(d).queue_sequence == before + 2 &&
          queue.sequence == before + 2);
    CHECK(resume(kernel, group, 1) == i64{0} && !native::futex_objects.resolve(a));
    // A moved wait that is cancelled is unlinked from the destination by
    // its owner's resume.
    CHECK(kernel.scheduler().complete_wait(record(b).ticket, sched::WaitCompletion::cancelled));
    CHECK(resume(kernel, group, 2) == i64{-4} && !native::futex_objects.resolve(b));
    CHECK(queue.head == &record(c) && record(c).bucket_next == &record(d));
    // FIFO on the destination: C, then D; nothing woke on the source key.
    CHECK(wake(kernel, group, source, 5) == 0);
    CHECK(wake(kernel, group, destination, 1) == 1 && completion(kernel, group, 3) == sched::WaitCompletion::ready);
    CHECK(wake(kernel, group, destination, 1) == 1 && completion(kernel, group, 4) == sched::WaitCompletion::ready);
    CHECK(resume(kernel, group, 3) == i64{0} && resume(kernel, group, 4) == i64{0});
    CHECK(!queue.head && !queue.tail);
    retire(kernel, group);
}

constexpr u32 wake_operation(u32 op, u32 cmp, u32 oparg, u32 cmparg)
{
    return (op << 28) | (cmp << 24) | ((oparg & 0xfffU) << 12) | (cmparg & 0xfffU);
}

// Two keys in one bucket: WAKE_OP and CMP_REQUEUE take that bucket once
// (a second take would be a reported self-deadlock), and a different-bucket
// pair takes the higher bucket through lock_as.
void equal_bucket_pair(Kernel &kernel)
{
    const auto group = owner(kernel, 6);
    const auto [x, y] = addresses(group, true);
    const auto index = bucket_of(group, x);
    CHECK(x != y && bucket_of(group, y) == index);
    put(x, 3); put(y, 5);
    const auto a = wait(kernel, group, 1, x), b = wait(kernel, group, 2, y);
    const auto violations = smp::lock_violation_count(0);
    activate(kernel, group);
    // y += 1; y was 5, which the comparison (EQ 5) accepts: both wake.
    CHECK(native::futex_wake_operation(kernel, group.pid, {x, y, 1, 1, wake_operation(1, 0, 1, 5)}) == 2);
    CHECK(get(y) == 6 && smp::lock_violation_count(0) == violations);
    CHECK(resume(kernel, group, 1) == i64{0} && resume(kernel, group, 2) == i64{0});
    CHECK(!native::futex_objects.resolve(a) && !native::futex_objects.resolve(b));
    // CMP_REQUEUE within the bucket: the moved wait goes behind the
    // destination's waiter and keeps FIFO order.
    const auto c = wait(kernel, group, 3, x), d = wait(kernel, group, 4, y);
    activate(kernel, group);
    CHECK(native::futex_requeue(kernel, group.pid, {x, y, 0, 1, 4, true, false, Domain::private_address_space}) == -11);
    CHECK(native::futex_requeue(kernel, group.pid, {x, y, 0, 1, 3, true, false, Domain::private_address_space}) == 1);
    CHECK(smp::lock_violation_count(0) == violations);
    CHECK(record(c).key.address == y && record(c).bucket == index && record(d).bucket_next == &record(c));
    CHECK(record(c).queue_sequence > record(d).queue_sequence);
    CHECK(wake(kernel, group, y, 1) == 1 && completion(kernel, group, 4) == sched::WaitCompletion::ready);
    CHECK(completion(kernel, group, 3) == sched::WaitCompletion::none);
    CHECK(wake(kernel, group, y, 1) == 1 && completion(kernel, group, 3) == sched::WaitCompletion::ready);
    CHECK(resume(kernel, group, 3) == i64{0} && resume(kernel, group, 4) == i64{0});
    // A different-bucket pair: lock_as records the higher one.
    const auto [p, q] = addresses(group, false);
    put(p, 1); put(q, 1);
    const auto e = wait(kernel, group, 5, p);
    activate(kernel, group);
    CHECK(native::futex_wake_operation(kernel, group.pid, {q, p, 1, 1, wake_operation(0, 0, 2, 1)}) == 1);
    CHECK(get(p) == 2 && smp::lock_violation_count(0) == violations);
    CHECK(resume(kernel, group, 5) == i64{0} && !native::futex_objects.resolve(e));
    retire(kernel, group);
}

// Host-thread races on the bucket list (real futex_bucket spin locks). The
// model wait completes by an exchange on its state, as complete_wait's
// pending check under the task lock does: the first completer wins.
struct ModelWait
{
    ModelWait *bucket_prev{}, *bucket_next{};
    u32 bucket{none};
    u64 queue_sequence{};
    u32 key{};
    std::atomic<u32> state{0}; // 0 pending, 1 woken, 2 interrupted, 3 cancelled.
};
using ModelBuckets = native::FutexBuckets<ModelWait, smp::RankedSpinLock<smp::LockRank::futex_bucket>, 4>;
bool complete(ModelWait &wait, u32 how)
{
    u32 pending = 0;
    return wait.state.compare_exchange_strong(pending, how);
}
// WAKE: unlinks each match it visits, counts those it completes.
usize model_wake(ModelBuckets &buckets, u32 index, u32 key, usize limit)
{
    const ModelBuckets::Guard guard{buckets, index};
    usize woken = 0;
    for (auto *wait = buckets[index].head; wait && woken < limit;)
    {
        auto *next = wait->bucket_next;
        if (wait->key == key)
        {
            buckets.unlink(index, *wait);
            if (complete(*wait, 1)) ++woken;
        }
        wait = next;
    }
    return woken;
}
// A short, round-dependent delay that spreads the interleavings.
void delay(usize round)
{
    std::atomic<usize> spin{0};
    for (usize i = 0; i < (round * 7) % 257; ++i) spin.fetch_add(1, std::memory_order_relaxed);
}
template <typename F> void on_threads(F &&first, F &&second)
{
    std::atomic<u32> ready{0};
    std::thread one([&] { const smp::ScopedHostedCpu cpu{1}; ready.fetch_add(1); while (ready.load() < 2) {} first(); });
    std::thread two([&] { const smp::ScopedHostedCpu cpu{2}; ready.fetch_add(1); while (ready.load() < 2) {} second(); });
    one.join();
    two.join();
}

// Compare-and-queue under the bucket against a waker that changes the word
// and then wakes: the waiter either sees the change or is found and woken.
usize race_compare_and_queue(usize rounds)
{
    static ModelBuckets buckets;
    usize queued = 0, saw = 0;
    for (usize round = 0; round < rounds; ++round)
    {
        ModelWait wait{};
        wait.key = 1;
        std::atomic<u32> word{0};
        bool waiter_queued = false;
        std::function<void()> waiter = [&] {
            {
                const ModelBuckets::Guard guard{buckets, 0};
                if (word.load(std::memory_order_relaxed) != 0) return;
                buckets.link_tail(0, wait);
                waiter_queued = true;
            }
            const auto start = std::chrono::steady_clock::now();
            while (wait.state.load() == 0)
                CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5)); // A lost wake.
        };
        std::function<void()> waker = [&] {
            delay(round);
            word.store(1, std::memory_order_relaxed);
            static_cast<void>(model_wake(buckets, 0, 1, 1));
        };
        on_threads(waiter, waker);
        CHECK(ModelBuckets::linked_bucket(wait) == none && !buckets[0].head);
        if (waiter_queued) { ++queued; CHECK(wait.state.load() == 1); }
        else ++saw;
    }
    ++rounds_checked;
    std::printf("race compare_and_queue rounds=%zu queued=%zu saw_change=%zu lost=0\n", rounds, queued, saw);
    return queued;
}

// A requeue moving a wait between buckets while its owner unlinks it: the
// owner follows the move and unlinks it exactly once, never seeing none
// while it is still listed.
void race_requeue_against_owner(usize rounds)
{
    static ModelBuckets buckets;
    usize moves = 0;
    for (usize round = 0; round < rounds; ++round)
    {
        ModelWait wait{};
        wait.key = 2;
        {
            const ModelBuckets::Guard guard{buckets, 1};
            buckets.link_tail(1, wait);
        }
        std::atomic<bool> owner_done{false};
        std::function<void()> mover = [&] {
            for (u32 i = 0; i < 16 && !owner_done.load(); ++i)
            {
                const u32 from = ModelBuckets::linked_bucket(wait);
                if (from == none) break;
                const u32 to = from == 1 ? 3 : 1;
                const ModelBuckets::PairGuard pair{buckets, from, to};
                if (ModelBuckets::linked_bucket(wait) != from) continue;
                buckets.move(from, to, wait);
                ++moves;
            }
        };
        std::function<void()> owner_side = [&] {
            delay(round);
            const auto index = buckets.lock_linked(wait);
            CHECK(index != none);
            buckets.unlink(index, wait);
            buckets[index].lock.unlock();
            owner_done.store(true);
        };
        on_threads(mover, owner_side);
        CHECK(ModelBuckets::linked_bucket(wait) == none);
        CHECK(!buckets[1].head && !buckets[1].tail && !buckets[3].head && !buckets[3].tail);
    }
    ++rounds_checked;
    std::printf("race requeue_against_owner rounds=%zu moves=%zu\n", rounds, moves);
}

// A signal's interrupt (owner unlink, then its exchange) against WAKE(1)
// on two queued waits: exactly one completes the first; when the signal
// wins, the wake goes to the second, so no wake is lost.
void race_interrupt_against_wake(usize rounds)
{
    static ModelBuckets buckets;
    usize interrupted = 0, woken_first = 0;
    for (usize round = 0; round < rounds; ++round)
    {
        ModelWait first{}, second{};
        first.key = second.key = 3;
        {
            const ModelBuckets::Guard guard{buckets, 2};
            buckets.link_tail(2, first);
            buckets.link_tail(2, second);
        }
        usize woke = 0;
        std::function<void()> waker = [&] { woke = model_wake(buckets, 2, 3, 1); };
        std::function<void()> signal = [&] {
            delay(round);
            const auto index = buckets.lock_linked(first);
            if (index == none) return; // The waker took it.
            buckets.unlink(index, first);
            static_cast<void>(complete(first, 2));
            buckets[index].lock.unlock();
        };
        on_threads(waker, signal);
        CHECK(woke == 1);
        const auto state = first.state.load();
        CHECK(state == 1 || state == 2);
        if (state == 2) { ++interrupted; CHECK(second.state.load() == 1); }
        else { ++woken_first; CHECK(second.state.load() == 0); }
        // The second wait's owner unlinks it if the wake left it queued.
        if (const auto index = buckets.lock_linked(second); index != none)
        {
            buckets.unlink(index, second);
            buckets[index].lock.unlock();
        }
        CHECK(!buckets[2].head && !buckets[2].tail);
    }
    ++rounds_checked;
    std::printf("race interrupt_against_wake rounds=%zu interrupted=%zu woken_first=%zu lost=0\n", rounds,
                interrupted, woken_first);
}
} // namespace

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    native::runtime_kernel = &kernel;
    timed_out_against_wake(kernel);
    signal_against_wake(kernel);
    requeue_across_buckets(kernel);
    equal_bucket_pair(kernel);
    CHECK(native::runtime_status());
    CHECK(smp::lock_stat_total(smp::LockStat::futex, false) > 0);
    expect_clean_locks();
    for (u32 index = 0; index < native::futex_bucket_count; ++index)
        CHECK(!native::futex_buckets[index].head && !native::futex_buckets[index].tail);
    race_compare_and_queue(20000);
    race_requeue_against_owner(20000);
    race_interrupt_against_wake(20000);
    expect_clean_locks();
    std::printf("OK_FUTEX_BUCKETS scope=model buckets=%zu runtime=pass races=%zu guest=0 hardware_smp=0\n",
                native::futex_bucket_count, rounds_checked);
}
