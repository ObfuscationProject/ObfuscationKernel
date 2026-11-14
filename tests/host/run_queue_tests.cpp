#include "ok/sched/run_queue.hpp"
#include "ok/sched/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace ok;
using namespace ok::sched;
static usize checks;
#define CHECK(condition) do { ++checks; if (!static_cast<bool>(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

// A lock whose state is visible, so a contended try_lock can be forced. Only
// one queue in this suite uses it.
struct ObservedLock
{
    static inline bool held{};
    static inline usize acquisitions{};
    void lock() { held = true; ++acquisitions; }
    void unlock() { held = false; }
    bool try_lock()
    {
        if (held) return false;
        lock();
        return true;
    }
};

static_assert(RunQueue<CpuRunQueue<8>>);
static_assert(RunQueue<CpuRunQueue<8, ObservedLock>>);
static_assert(RunQueue<SchedulerRunQueue>);
static_assert(SchedulerRunQueue::capacity() == max_processes * max_threads_per_process);
static_assert(!std::is_copy_constructible_v<CpuRunQueue<8>>);
static_assert(!std::is_polymorphic_v<Scheduler>);
// Kernel globals run no dynamic initializers.
constinit CpuRunQueue<8> constinit_queue;
constinit smp::PerCpu<SchedulerRunQueue> constinit_queues;

void test_queue_fifo_overflow_and_removal()
{
    CHECK(constinit_queue.empty() && constinit_queues.get(smp::max_cpus - 1).empty());
    auto queue = std::make_unique<CpuRunQueue<4, ObservedLock>>();
    CHECK(queue->empty() && !queue->pop());
    for (u64 id = 1; id <= 4; ++id)
        CHECK(queue->push({id, id * 10}));
    CHECK(queue->push({5, 50}).code() == StatusCode::overflow);
    CHECK(queue->size() == 4 && !queue->contains({5, 50}));
    CHECK(queue->contains({3, 30}) && !queue->contains({3, 31}));
    CHECK(queue->remove({2, 20}) && !queue->remove({2, 20}) && queue->size() == 3);
    const auto first = queue->pop();
    CHECK(first && *first == (ThreadRef{1, 10}));
    // Wrap the ring several times; FIFO order survives removal in the middle.
    for (u64 id = 6; id <= 40; ++id)
    {
        CHECK(queue->push({id, id * 10}));
        CHECK(queue->size() == 3);
        const auto popped = queue->pop();
        CHECK(popped && popped->pid == id - (id == 6 ? 3 : id == 7 ? 3 : 2));
    }
    CHECK(queue->size() == 2);
    CHECK(queue->push({41, 410}) && queue->push({42, 420}));
    CHECK(queue->remove({40, 400}));
    std::array<u64, 3> order{};
    for (auto &pid : order)
    {
        const auto popped = queue->pop();
        CHECK(popped);
        pid = popped->pid;
    }
    CHECK(order == (std::array<u64, 3>{39, 41, 42}) && queue->empty() && !ObservedLock::held);

    CHECK(queue->push({1, 1}) && queue->push({2, 2}) && queue->push({3, 3}));
    const auto taken = queue->try_take_first([](ThreadRef ref) { return ref.pid >= 2; });
    CHECK(taken && taken->pid == 2 && queue->size() == 2 && !ObservedLock::held);
    CHECK(!queue->try_take_first([](ThreadRef) { return false; }) && queue->size() == 2);
    // A contended victim is skipped rather than waited for.
    ObservedLock::held = true;
    const auto before = ObservedLock::acquisitions;
    CHECK(!queue->try_take_first(AnyThreadRef{}) && queue->size() == 2 && ObservedLock::acquisitions == before);
    ObservedLock::held = false;
    const auto oldest = queue->pop();
    CHECK(oldest && oldest->pid == 1);
}

struct Fixture
{
    std::unique_ptr<Scheduler> owned{std::make_unique<Scheduler>()};
    Scheduler &scheduler{*owned};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(0x1000, 0x8000)};

    explicit Fixture(usize cpus = 4, u32 online = 0xf)
    {
        CHECK(scheduler.configure_cpus(cpus));
        CHECK(scheduler.set_online_cpus(online));
    }
    // Unless `fresh`, the thread has a history on CPU 0, which placement
    // prefers while its affinity allows CPU 0.
    ProcessId create(u16 affinity = cpu_affinity_any, bool fresh = false)
    {
        auto process = scheduler.create_process("queued", context);
        CHECK(process);
        CHECK(scheduler.set_runnable(process.value()));
        if (affinity != cpu_affinity_any)
            CHECK(scheduler.set_cpu_affinity(process.value(), affinity));
        if (!fresh)
            thread(process.value()).dispatch_count = 1;
        return process.value();
    }
    // A further thread with a history on CPU 0, like create().
    Result<ThreadId> create_thread(ProcessId pid)
    {
        auto created = scheduler.create_thread(pid, context);
        if (created)
            thread(pid, created.value()).dispatch_count = 1;
        return created;
    }
    ThreadControlBlock &thread(ProcessId pid, ThreadId tid = 0)
    {
        auto *found = scheduler.find_thread(pid, tid ? tid : pid);
        CHECK(found);
        return *found;
    }
    smp::CpuId enqueue(ProcessId pid, ThreadId tid = 0)
    {
        CHECK(scheduler.enqueue_thread(pid, tid ? tid : pid));
        return thread(pid, tid).queued_cpu;
    }
    ProcessId dequeue(smp::CpuId cpu)
    {
        const auto ref = scheduler.dequeue_thread(cpu);
        if (!ref) return 0;
        CHECK(thread(ref->pid, ref->tid).queued_cpu == no_cpu);
        return ref->pid;
    }
    usize length(smp::CpuId cpu) const { return scheduler.run_queue(cpu).size(); }
    // Every live queue entry is recorded by its thread, and every recorded entry exists once.
    void check_consistent()
    {
        usize entries = 0;
        for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
            entries += length(cpu);
        usize recorded = 0;
        for (const auto &process : scheduler.processes())
            for (const auto &thread : process.threads())
                if (thread.queued_cpu != no_cpu)
                {
                    ++recorded;
                    CHECK(scheduler.run_queue(thread.queued_cpu).contains({process.pid(), thread.tid}));
                }
        CHECK(recorded == entries);
    }
};

void test_online_mask()
{
    auto owned = std::make_unique<Scheduler>();
    auto &scheduler = *owned;
    CHECK(scheduler.online_cpus() == 1);
    CHECK(scheduler.configure_cpus(4) && scheduler.online_cpus() == 1);
    CHECK(scheduler.set_online_cpus(0x2).code() == StatusCode::invalid_argument);
    CHECK(scheduler.set_online_cpus(0x1f).code() == StatusCode::invalid_argument);
    CHECK(scheduler.set_online_cpus(0xf) && scheduler.online_cpus() == 0xf);
    CHECK(scheduler.configure_cpus(2) && scheduler.online_cpus() == 0x3);
    CHECK(!scheduler.dequeue_thread(2) && !scheduler.dequeue_thread(smp::max_cpus));
}

void test_placement()
{
    Fixture f;
    const auto first = f.create();
    CHECK(f.enqueue(first) == 0);
    CHECK(f.scheduler.enqueue_thread(first, first) && f.length(0) == 1);
    CHECK(f.scheduler.enqueue_thread(first, 999).code() == StatusCode::not_found);
    CHECK(f.scheduler.enqueue_thread(999, 999).code() == StatusCode::not_found);

    // The last CPU wins while it is online and allowed.
    const auto warm = f.create();
    CHECK(f.scheduler.mark_on_cpu(warm, warm, 2));
    CHECK(f.scheduler.enqueue_thread(warm, warm) && f.length(2) == 0 && f.thread(warm).queued_cpu == no_cpu);
    CHECK(f.scheduler.clear_on_cpu(warm, warm, 2) && f.thread(warm).last_cpu == 2);
    CHECK(f.enqueue(warm) == 2);

    // Affinity excludes the last CPU: shortest allowed queue, lowest id on a tie.
    const auto pinned = f.create(0x8);
    CHECK(f.enqueue(pinned) == 3);
    const auto spread = f.create(0xe);
    CHECK(f.enqueue(spread) == 1);
    const auto tie = f.create(0xe);
    CHECK(f.enqueue(tie) == 1);
    const auto shortest = f.create(0xe);
    CHECK(f.enqueue(shortest) == 2);
    CHECK(f.length(0) == 1 && f.length(1) == 2 && f.length(2) == 2 && f.length(3) == 1);

    // No allowed CPU online: never an offline queue. CPU 0 keeps the entry
    // but does not run it; a CPU it allows takes it once online.
    CHECK(f.scheduler.set_online_cpus(0x3));
    const auto waiting = f.create(0xc);
    CHECK(f.enqueue(waiting) == 0);
    CHECK(f.dequeue(1) == spread && f.dequeue(1) == tie && f.dequeue(1) == first);
    CHECK(f.dequeue(1) == 0 && f.dequeue(0) == 0 && f.length(2) == 2);
    CHECK(f.length(0) == 1 && f.thread(waiting).queued_cpu == 0);
    CHECK(f.scheduler.set_online_cpus(0xf));
    CHECK(f.dequeue(2) == warm);
    CHECK(f.dequeue(3) == pinned && f.dequeue(3) == waiting && f.length(0) == 0);
    f.check_consistent();

    // A thread that is not runnable is refused; one on a CPU is left alone.
    const auto blocked = f.create();
    CHECK(f.scheduler.block_process(blocked));
    CHECK(f.scheduler.enqueue_thread(blocked, blocked).code() == StatusCode::invalid_argument);
    CHECK(f.thread(blocked).queued_cpu == no_cpu);

    // Threads that never ran spread over the online queues.
    Fixture fresh;
    for (unsigned i = 0; i < 4; ++i)
        CHECK(fresh.enqueue(fresh.create(cpu_affinity_any, true)) == i);
    CHECK(fresh.length(0) == 1 && fresh.length(1) == 1 && fresh.length(2) == 1 && fresh.length(3) == 1);
}

void test_stealing()
{
    Fixture f;
    const auto pinned = f.create(0x1);
    const auto movable = f.create();
    const auto other = f.create();
    CHECK(f.enqueue(pinned) == 0 && f.enqueue(movable) == 0 && f.enqueue(other) == 0);
    // Steals the oldest entry the thief may run; the victim keeps the rest in order.
    CHECK(f.dequeue(1) == movable);
    CHECK(f.length(0) == 2 && f.thread(pinned).queued_cpu == 0 && f.thread(other).queued_cpu == 0);
    CHECK(f.scheduler.run_queue(0).contains({pinned, pinned}) && f.scheduler.run_queue(0).contains({other, other}));
    CHECK(f.dequeue(2) == other);
    CHECK(f.dequeue(3) == 0 && f.length(0) == 1 && f.thread(pinned).queued_cpu == 0);
    CHECK(f.dequeue(0) == pinned && f.dequeue(0) == 0);
    f.check_consistent();

    // The longest victim is tried first; an ineligible one falls back to the next.
    std::array<ProcessId, 3> crowd{};
    for (auto &pid : crowd)
    {
        pid = f.create(0x4);
        CHECK(f.enqueue(pid) == 2);
    }
    const auto lone = f.create(0x8);
    CHECK(f.enqueue(lone) == 3);
    CHECK(f.scheduler.set_cpu_affinity(lone, cpu_affinity_any));
    CHECK(f.dequeue(1) == lone && f.length(2) == 3);
    for (const auto pid : crowd)
        CHECK(f.scheduler.set_cpu_affinity(pid, cpu_affinity_any));
    CHECK(f.dequeue(1) == crowd[0] && f.length(2) == 2);
    // Offline queues are never victims.
    CHECK(f.scheduler.set_online_cpus(0x3));
    CHECK(f.dequeue(1) == 0 && f.length(2) == 2);
    f.check_consistent();
}

void test_stale_entries()
{
    Fixture f;
    const auto sleeper = f.create();
    const auto runner = f.create();
    CHECK(f.enqueue(sleeper) == 0 && f.enqueue(runner) == 0);
    CHECK(f.scheduler.block_process(sleeper));
    // The blocked entry is discarded and its record cleared.
    CHECK(f.dequeue(0) == runner && f.thread(sleeper).queued_cpu == no_cpu && f.length(0) == 0);

    // An entry that went stale and became runnable again is still the one entry.
    CHECK(f.enqueue(runner) == 0);
    CHECK(f.scheduler.block_process(runner));
    CHECK(f.scheduler.set_runnable(runner));
    CHECK(f.enqueue(runner) == 0 && f.length(0) == 1);
    CHECK(f.dequeue(0) == runner && f.length(0) == 0);

    // A changed affinity moves the entry instead of dropping it.
    CHECK(f.enqueue(runner) == 0);
    CHECK(f.scheduler.set_cpu_affinity(runner, 0x2));
    CHECK(f.dequeue(0) == 0 && f.thread(runner).queued_cpu == 1 && f.length(1) == 1);
    CHECK(f.dequeue(1) == runner);

    // Dispatch through the policy path drops the dispatched thread's entry.
    CHECK(f.scheduler.set_runnable(sleeper));
    CHECK(f.enqueue(sleeper) == 0);
    CHECK(f.scheduler.activate(sleeper, 0) && f.length(0) == 0 && f.thread(sleeper).queued_cpu == no_cpu);
    f.check_consistent();
}

void test_exit_and_erase_clean_queues()
{
    Fixture f;
    const auto group = f.create();
    const auto worker = f.create_thread(group);
    CHECK(worker);
    CHECK(f.enqueue(group) == 0 && f.enqueue(group, worker.value()) == 0);
    CHECK(f.scheduler.exit_thread(group, worker.value()));
    CHECK(f.length(0) == 1 && !f.scheduler.run_queue(0).contains({group, worker.value()}));

    // Exec retires every other thread and its entry.
    const auto second = f.create_thread(group);
    CHECK(second && f.enqueue(group, second.value()) == 0 && f.length(0) == 2);
    CHECK(f.scheduler.replace_image(group, f.context, {}));
    CHECK(f.length(0) == 1 && f.scheduler.run_queue(0).contains({group, group}));

    // Process exit removes the entries; reaping compacts storage under live entries.
    const auto survivor = f.create();
    CHECK(f.enqueue(survivor) == 0);
    CHECK(f.scheduler.exit_process(group, 0));
    CHECK(f.length(0) == 1 && !f.scheduler.run_queue(0).contains({group, group}));
    CHECK(f.scheduler.reap_process(group));
    CHECK(f.scheduler.find(group) == nullptr && f.scheduler.processes()[0].pid() == survivor);
    CHECK(f.dequeue(0) == survivor);

    // kill_process exits and erases in one step.
    const auto doomed = f.create();
    CHECK(f.enqueue(doomed) == 0);
    CHECK(f.scheduler.kill_process(doomed) && f.length(0) == 0);

    // A retired thread slot reused by create_thread carries no entry.
    const auto host = f.create();
    const auto retiring = f.create_thread(host);
    CHECK(retiring && f.enqueue(host, retiring.value()) == 0);
    CHECK(f.scheduler.exit_thread(host, retiring.value()) && f.length(0) == 0);
    const auto reused = f.create_thread(host);
    CHECK(reused && f.thread(host, reused.value()).queued_cpu == no_cpu);
    f.check_consistent();

    // Shrinking the CPU set moves entries off removed CPUs.
    const auto far = f.create(0x8);
    CHECK(f.enqueue(far) == 3);
    const auto near = f.create(0x4);
    CHECK(f.enqueue(near) == 2);
    CHECK(f.scheduler.set_cpu_affinity(near, cpu_affinity_any));
    CHECK(f.scheduler.configure_cpus(2) && f.scheduler.online_cpus() == 0x3);
    CHECK(f.length(2) == 0 && f.length(3) == 0);
    CHECK(f.thread(far).queued_cpu == 0 && f.thread(near).queued_cpu < 2);
    // An affinity naming no configured CPU runs on CPU 0.
    CHECK(f.scheduler.dequeue_thread(1).value_or(ThreadRef{}).pid != far);
    while (const auto ref = f.scheduler.dequeue_thread(0))
        CHECK(ref->pid == far || ref->pid == near || ref->pid == host);
    CHECK(f.scheduler.mark_on_cpu(far, far, 0));
    f.check_consistent();
}

void test_mark_on_cpu()
{
    Fixture f(4, 0x7);
    const auto owner = f.create();
    const auto rival = f.create();
    CHECK(f.enqueue(owner) == 0);
    CHECK(f.scheduler.mark_on_cpu(owner, owner, 1));
    auto &thread = f.thread(owner);
    CHECK(thread.on_cpu == 1 && thread.last_cpu == 1 && thread.queued_cpu == no_cpu && f.length(0) == 0);
    CHECK(thread.state == ProcessState::running && f.scheduler.find(owner)->state() == ProcessState::running);
    CHECK(f.scheduler.current_pid(1) == owner && f.scheduler.current_tid(1) == owner);
    CHECK(f.scheduler.mark_on_cpu(owner, owner, 1));
    CHECK(f.scheduler.mark_on_cpu(owner, owner, 2).code() == StatusCode::busy);
    CHECK(f.scheduler.mark_on_cpu(rival, rival, 1).code() == StatusCode::busy);
    CHECK(f.scheduler.mark_on_cpu(rival, rival, 3).code() == StatusCode::invalid_argument);
    CHECK(f.scheduler.mark_on_cpu(rival, 999, 2).code() == StatusCode::not_found);
    CHECK(f.scheduler.enqueue_thread(owner, owner) && thread.queued_cpu == no_cpu);
    // The previous slot scan is now the on_cpu record; its callers keep their answers.
    CHECK(!f.scheduler.activate_thread(owner, owner, 2));
    CHECK(f.scheduler.exit_thread(owner, owner, 2).status().code() == StatusCode::busy);
    CHECK(f.scheduler.clear_on_cpu(owner, owner, 2).code() == StatusCode::invalid_argument);
    CHECK(f.scheduler.clear_on_cpu(owner, owner, 1));
    CHECK(thread.on_cpu == no_cpu && thread.state == ProcessState::runnable && f.scheduler.current_pid(1) == 0);
    CHECK(f.scheduler.find(owner)->state() == ProcessState::runnable);
    CHECK(f.scheduler.mark_on_cpu(owner, owner, 2) && f.scheduler.current_tid(2) == owner);

    // A blocked thread cannot be marked.
    CHECK(f.scheduler.block_process(rival));
    CHECK(f.scheduler.mark_on_cpu(rival, rival, 1).code() == StatusCode::invalid_argument);
    // Blocking the owner vacates its CPU slot and record.
    CHECK(f.scheduler.block_process(owner) && thread.on_cpu == no_cpu && f.scheduler.current_pid(2) == 0);
    f.check_consistent();
}

void test_two_threads_one_process()
{
    Fixture f(2, 0x3);
    const auto group = f.create();
    const auto peer = f.create_thread(group);
    const auto other = f.create();
    const auto third = f.create();
    CHECK(peer);
    CHECK(f.scheduler.activate_thread(group, group, 0) && f.scheduler.activate_thread(group, peer.value(), 1));
    CHECK(f.thread(group).on_cpu == 0 && f.thread(group, peer.value()).on_cpu == 1);
    // Preempting one thread keeps the process running while its peer runs.
    CHECK(f.scheduler.activate(other, 0));
    CHECK(f.thread(group).state == ProcessState::runnable && f.thread(group).on_cpu == no_cpu);
    CHECK(f.scheduler.find(group)->state() == ProcessState::running);
    CHECK(f.scheduler.activate(third, 1));
    CHECK(f.scheduler.find(group)->state() == ProcessState::runnable);
    CHECK(f.thread(group, peer.value()).state == ProcessState::runnable);

    // The queue-driven pair behaves the same.
    CHECK(f.scheduler.activate(0, 0) && f.scheduler.activate(0, 1));
    CHECK(f.scheduler.mark_on_cpu(group, group, 0) && f.scheduler.mark_on_cpu(group, peer.value(), 1));
    CHECK(f.scheduler.clear_on_cpu(group, group, 0));
    CHECK(f.scheduler.find(group)->state() == ProcessState::running);
    CHECK(f.scheduler.clear_on_cpu(group, peer.value(), 1));
    CHECK(f.scheduler.find(group)->state() == ProcessState::runnable);

    // One CPU: preemption makes the process runnable exactly as before.
    Fixture single(1, 0x1);
    const auto first = single.create();
    const auto second = single.create();
    CHECK(single.scheduler.activate(first, 0) && single.scheduler.activate(second, 0));
    CHECK(single.scheduler.find(first)->state() == ProcessState::runnable);
    CHECK(single.thread(first).state == ProcessState::runnable && single.thread(first).on_cpu == no_cpu);
    CHECK(single.thread(second).on_cpu == 0 && single.scheduler.current_pid() == second);
}

// CPU hotplug: a CPU leaving moves its queue to online CPUs, keeps no entry
// and is never selected; every runnable thread stays queued exactly once.
void test_hotplug()
{
    Fixture f;
    CHECK(f.scheduler.offline_cpu(0).code() == StatusCode::invalid_argument);
    CHECK(f.scheduler.offline_cpu(4).code() == StatusCode::invalid_argument);
    CHECK(f.scheduler.online_cpu(4).code() == StatusCode::invalid_argument);
    std::vector<ProcessId> all{};
    for (const u16 affinity : {cpu_affinity_any, u16{0x8}, u16{0xc}, u16{0x2}, cpu_affinity_any, u16{0x8}})
        all.push_back(f.create(affinity, true));
    for (const auto pid : all)
        CHECK(f.enqueue(pid) < 4);
    const auto pinned = all[1];
    CHECK(f.thread(pinned).queued_cpu == 3 && f.length(3) >= 2);
    // A thread running on the leaving CPU keeps its slot until it vacates it.
    const auto running = f.create(cpu_affinity_any, true);
    CHECK(f.scheduler.mark_on_cpu(running, running, 3));
    // A stale entry (its thread blocked) is dropped, not placed again.
    const auto stale = f.create(0x8, true);
    CHECK(f.enqueue(stale) == 3);
    CHECK(f.scheduler.block_process(stale));

    CHECK(f.scheduler.offline_cpu(3) && f.scheduler.online_cpus() == 0x7);
    CHECK(f.length(3) == 0 && f.thread(stale).queued_cpu == no_cpu);
    for (const auto pid : all)
        CHECK(f.thread(pid).queued_cpu < 3);
    CHECK(f.thread(pinned).queued_cpu == 0 && f.thread(all[5]).queued_cpu == 0);
    f.check_consistent();
    CHECK(f.thread(running).on_cpu == 3 && f.scheduler.current_tid(3) == running);
    CHECK(!f.scheduler.dequeue_thread(3));
    CHECK(f.scheduler.mark_on_cpu(all[0], all[0], 3).code() == StatusCode::invalid_argument);
    // Its last CPU went offline: placement picks an online one.
    CHECK(f.scheduler.clear_on_cpu(running, running, 3) && f.thread(running).last_cpu == 3);
    CHECK(f.enqueue(running) < 3);

    // Dequeue everything the online CPUs may run: each thread exactly once,
    // and the threads pinned to CPU 3 wait on CPU 0.
    std::vector<ProcessId> taken{};
    for (bool progress = true; progress;)
    {
        progress = false;
        for (smp::CpuId cpu = 0; cpu < 3; ++cpu)
            if (const auto pid = f.dequeue(cpu))
            {
                CHECK(pid != pinned && pid != all[5]);
                taken.push_back(pid);
                progress = true;
            }
    }
    std::sort(taken.begin(), taken.end());
    CHECK(std::adjacent_find(taken.begin(), taken.end()) == taken.end() && taken.size() == all.size() - 2 + 1);
    CHECK(f.length(0) == 2 && f.thread(pinned).queued_cpu == 0);
    f.check_consistent();

    // Back online: CPU 3 takes its pinned threads from CPU 0.
    CHECK(f.scheduler.online_cpu(3) && f.scheduler.online_cpus() == 0xf);
    const auto first = f.dequeue(3), second = f.dequeue(3);
    CHECK(((first == pinned && second == all[5]) || (first == all[5] && second == pinned)) && f.dequeue(3) == 0);
    f.check_consistent();

    // Repeated cycles of every secondary with traffic in between: no entry on
    // an offline queue, no placement on an offline CPU, nothing lost or doubled.
    Fixture g;
    std::vector<ProcessId> threads{};
    for (unsigned i = 0; i < 24; ++i)
        threads.push_back(g.create(i % 5 == 0 ? u16(1u << (i % 4)) : cpu_affinity_any, i % 3 == 0));
    u32 online = 0xf;
    unsigned seed = 7;
    std::vector<ProcessId> running_set{};
    for (unsigned round = 0; round < 400; ++round)
    {
        seed = seed * 1103515245u + 12345u;
        const auto cpu = static_cast<smp::CpuId>(1 + (seed >> 16) % 3);
        if ((online & (1u << cpu)) != 0)
        {
            CHECK(g.scheduler.offline_cpu(cpu));
            online &= ~(1u << cpu);
        }
        else
        {
            CHECK(g.scheduler.online_cpu(cpu));
            online |= 1u << cpu;
        }
        CHECK(g.scheduler.online_cpus() == online);
        for (const auto pid : threads)
            if (std::find(running_set.begin(), running_set.end(), pid) == running_set.end())
            {
                CHECK(g.scheduler.enqueue_thread(pid, pid));
                const auto placed = g.thread(pid).queued_cpu;
                CHECK(placed < 4 && (online & (1u << placed)) != 0);
                CHECK((online & (1u << g.scheduler.select_cpu(*g.scheduler.find(pid), g.thread(pid)))) != 0);
            }
        for (smp::CpuId other = 0; other < 4; ++other)
            if ((online & (1u << other)) == 0)
                CHECK(g.length(other) == 0 && !g.scheduler.dequeue_thread(other));
        g.check_consistent();
        // Run a few, then return them; each dequeued thread was queued once.
        for (smp::CpuId other = 0; other < 4; ++other)
            if (g.scheduler.current_tid(other) != 0)
                continue;
            else if (const auto pid = g.dequeue(other))
            {
                CHECK((online & (1u << other)) != 0);
                CHECK(std::find(running_set.begin(), running_set.end(), pid) == running_set.end());
                CHECK(g.scheduler.mark_on_cpu(pid, pid, other));
                running_set.push_back(pid);
            }
        usize queued = 0;
        for (const auto pid : threads)
            queued += g.thread(pid).queued_cpu != no_cpu;
        CHECK(queued + running_set.size() == threads.size());
        // Every other round the running threads leave their CPUs, offline or not.
        if (round % 2 == 1)
        {
            for (const auto pid : running_set)
                CHECK(g.scheduler.clear_on_cpu(pid, pid, g.thread(pid).on_cpu));
            running_set.clear();
        }
    }
    std::printf("hotplug threads=%zu rounds=400\n", threads.size());
}

// Host threads stand in for CPUs; each owns one queue and steals from the others.
void test_contention()
{
    constexpr usize workers = 4;
    constexpr u64 per_worker = 20000;
    auto queues = std::make_unique<smp::PerCpu<CpuRunQueue<256>, workers>>();
    std::atomic<usize> taken_total{0};
    std::array<std::vector<ThreadRef>, workers> taken{};
    std::array<std::thread, workers> threads{};
    for (usize worker = 0; worker < workers; ++worker)
        threads[worker] = std::thread([&, worker] {
            auto &own = queues->get(static_cast<smp::CpuId>(worker));
            u64 next = 0;
            while (taken_total.load(std::memory_order_acquire) < workers * per_worker)
            {
                for (usize burst = 0; burst < 2 && next < per_worker; ++burst)
                    if (own.push({worker + 1, next + 1}))
                        ++next;
                auto ref = own.pop();
                for (usize victim = 1; !ref && victim < workers; ++victim)
                    ref = queues->get(static_cast<smp::CpuId>((worker + victim) % workers)).try_take_first(AnyThreadRef{});
                if (ref)
                {
                    taken[worker].push_back(*ref);
                    taken_total.fetch_add(1, std::memory_order_acq_rel);
                }
            }
        });
    for (auto &thread : threads)
        thread.join();
    std::vector<ThreadRef> all{};
    usize stolen = 0;
    for (usize worker = 0; worker < workers; ++worker)
    {
        for (const auto ref : taken[worker])
            stolen += ref.pid != worker + 1;
        all.insert(all.end(), taken[worker].begin(), taken[worker].end());
    }
    CHECK(all.size() == workers * per_worker);
    std::sort(all.begin(), all.end(), [](ThreadRef left, ThreadRef right) {
        return left.pid != right.pid ? left.pid < right.pid : left.tid < right.tid;
    });
    usize exact = 0;
    for (usize index = 0; index < all.size(); ++index)
        exact += all[index] == (ThreadRef{index / per_worker + 1, index % per_worker + 1});
    CHECK(exact == workers * per_worker);
    for (usize worker = 0; worker < workers; ++worker)
        CHECK(queues->get(static_cast<smp::CpuId>(worker)).empty());
    std::printf("contention refs=%zu stolen=%zu\n", all.size(), stolen);
}

int main()
{
    test_queue_fifo_overflow_and_removal();
    test_online_mask();
    test_placement();
    test_stealing();
    test_stale_entries();
    test_exit_and_erase_clean_queues();
    test_mark_on_cpu();
    test_two_threads_one_process();
    test_hotplug();
    test_contention();
    std::printf("OK_RUN_QUEUES scope=model checks=%zu model_cpus=4 host_threads=4 guest=0 hardware_smp=0\n", checks);
}
