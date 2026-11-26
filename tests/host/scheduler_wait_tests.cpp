#include "ok/sched/scheduler.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ok;
using namespace ok::sched;
static usize checks;
#define CHECK(condition) do { ++checks; if (!static_cast<bool>(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

struct Fixture
{
    std::unique_ptr<Scheduler> owned{std::make_unique<Scheduler>()};
    Scheduler &scheduler{*owned};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(0x1000, 0x8000)};
    ProcessId create()
    {
        auto process = scheduler.create_process("wait-owner", context);
        CHECK(process);
        CHECK(scheduler.set_runnable(process.value()));
        return process.value();
    }
    WaitTicket wait(ProcessId pid, ThreadWaitReason reason = ThreadWaitReason::poll, u64 key = 17)
    {
        CHECK(scheduler.activate(pid));
        auto *thread = scheduler.current_thread();
        CHECK(thread && thread->owner == pid);
        auto ticket = scheduler.register_current_wait(reason, key);
        CHECK(ticket);
        CHECK(thread->wait_registration.pending(ticket.value()));
        CHECK(thread->state == ProcessState::blocked);
        return ticket.value();
    }
};

void test_identity_rearm_and_completion_order()
{
    Fixture f;
    CHECK(!f.scheduler.current_thread());
    CHECK(!f.scheduler.register_current_wait(ThreadWaitReason::none, 0));
    CHECK(!f.scheduler.register_current_wait(ThreadWaitReason::poll, 0));
    const auto owner = f.create(), other = f.create();
    const auto ticket = f.wait(owner);
    auto *thread = f.scheduler.find_thread(ticket.owner, ticket.thread);
    CHECK(thread && f.scheduler.current_thread() == thread);
    CHECK(!f.scheduler.register_current_wait(ThreadWaitReason::poll, 18));
    auto forged = ticket;
    forged.owner = other;
    CHECK(!f.scheduler.complete_wait(forged, WaitCompletion::ready));
    forged = ticket;
    forged.thread = f.scheduler.find(other)->leader().tid;
    CHECK(!f.scheduler.complete_wait(forged, WaitCompletion::ready));
    forged = ticket;
    ++forged.generation;
    CHECK(!f.scheduler.complete_wait(forged, WaitCompletion::timeout));
    CHECK(thread->wait_registration.pending(ticket));
    CHECK(thread->native_deadline.arm(10, 0, 20, false));
    const auto key = f.scheduler.find(ticket.owner)->thread_index(*thread);
    CHECK(f.scheduler.arm_wait_deadline(ticket, 30));
    CHECK(f.scheduler.deadline_of(key) == 30 && f.scheduler.earliest_deadline() == 30);
    CHECK(f.scheduler.complete_wait(ticket, WaitCompletion::ready));
    // The wait's heap key goes with it; the sleep record is the owner's.
    CHECK(f.scheduler.deadline_of(key) == DeadlineHeap<deadline_keys>::never);
    CHECK(f.scheduler.earliest_deadline() == DeadlineHeap<deadline_keys>::never);
    CHECK(thread->native_deadline.armed());
    thread->native_deadline.cancel();
    CHECK(thread->state == ProcessState::runnable && thread->wait_reason == ThreadWaitReason::none);
    CHECK(thread->wait_key == 0 && !thread->wait_registration.pending(ticket));
    CHECK(!f.scheduler.complete_wait(ticket, WaitCompletion::timeout));
    CHECK(!f.scheduler.complete_wait(ticket, WaitCompletion::cancelled));
    auto previous = ticket;
    for (usize iteration = 0; iteration < 64; ++iteration)
    {
        const auto next = f.wait(owner);
        CHECK(next.generation > previous.generation);
        CHECK(!f.scheduler.complete_wait(previous, WaitCompletion::ready));
        CHECK(!f.scheduler.complete_wait(previous, WaitCompletion::timeout));
        const auto first = iteration % 2 ? WaitCompletion::timeout : WaitCompletion::ready;
        const auto second = iteration % 2 ? WaitCompletion::ready : WaitCompletion::timeout;
        CHECK(f.scheduler.complete_wait(next, first));
        CHECK(!f.scheduler.complete_wait(next, second));
        previous = next;
    }
    const auto cancelled = f.wait(owner);
    CHECK(f.scheduler.complete_wait(cancelled, WaitCompletion::cancelled));
    CHECK(!f.scheduler.complete_wait(cancelled, WaitCompletion::ready));
    const auto administrative = f.wait(owner);
    CHECK(f.scheduler.set_runnable(owner));
    CHECK(!f.scheduler.complete_wait(administrative, WaitCompletion::timeout));
    CHECK(!thread->wait_registration.pending(administrative));
}

void test_stable_slots_and_lifecycle()
{
    Fixture f;
    const auto before = f.create(), owner = f.create(), after = f.create();
    const auto ticket = f.wait(owner);
    const auto old_address = reinterpret_cast<uptr>(f.scheduler.find(owner));
    CHECK(f.scheduler.exit_process(before, 0));
    CHECK(f.scheduler.reap_process(before));
    CHECK(f.scheduler.find(owner) && f.scheduler.find(after));
    // Reaping another process moves nothing.
    CHECK(reinterpret_cast<uptr>(f.scheduler.find(owner)) == old_address);
    CHECK(f.scheduler.find_thread(ticket.owner, ticket.thread)->wait_registration.pending(ticket));
    CHECK(f.scheduler.complete_wait(ticket, WaitCompletion::ready));
    const auto exiting = f.wait(owner);
    CHECK(f.scheduler.exit_process(owner, 42));
    CHECK(!f.scheduler.complete_wait(exiting, WaitCompletion::ready));
    CHECK(!f.scheduler.find_thread(exiting.owner, exiting.thread)->wait_registration.pending(exiting));
    CHECK(f.scheduler.reap_process(owner));
    CHECK(!f.scheduler.complete_wait(exiting, WaitCompletion::timeout));
    const auto replacement = f.create();
    CHECK(replacement != owner);
    const auto replacement_wait = f.wait(replacement);
    CHECK(!f.scheduler.complete_wait(exiting, WaitCompletion::ready));
    CHECK(f.scheduler.complete_wait(replacement_wait, WaitCompletion::ready));
}

void test_current_thread_exec_and_fork()
{
    Fixture f;
    const auto parent = f.create();
    const auto first_tid = f.scheduler.find(parent)->leader().tid;
    const auto second = f.scheduler.create_thread(parent, f.context);
    CHECK(second);
    CHECK(f.scheduler.activate(parent));
    CHECK(f.scheduler.current_tid(0) == first_tid);
    CHECK(f.scheduler.activate(parent));
    CHECK(f.scheduler.current_tid(0) == second.value());
    auto second_wait = f.scheduler.register_current_wait(ThreadWaitReason::poll, 23);
    CHECK(second_wait && second_wait.value().thread == second.value());
    CHECK(f.scheduler.find_thread(parent, first_tid)->state == ProcessState::runnable);
    CHECK(f.scheduler.find(parent)->state() != ProcessState::blocked);
    CHECK(f.scheduler.activate(parent));
    CHECK(f.scheduler.current_thread()->tid == first_tid);
    const auto child = f.scheduler.fork_process(parent);
    CHECK(child);
    auto &child_thread = f.scheduler.find(child.value())->leader();
    CHECK(child_thread.tid != first_tid && child_thread.owner == child.value());
    CHECK(!child_thread.wait_registration.pending(second_wait.value()));
    CHECK(child_thread.wait_reason == ThreadWaitReason::none && !child_thread.native_deadline.armed());
    CHECK(f.scheduler.find_thread(parent, second.value())->wait_registration.pending(second_wait.value()));
    CHECK(f.scheduler.complete_wait(second_wait.value(), WaitCompletion::timeout));
    // Re-arm the second thread, then exec from the first. Removed TIDs cannot complete.
    const auto second_again = f.wait(parent);
    CHECK(second_again.thread == second.value());
    const auto first_wait = f.wait(parent);
    CHECK(first_wait.thread == first_tid);
    CHECK(f.scheduler.find(parent)->state() == ProcessState::blocked);
    MemoryMap empty;
    CHECK(f.scheduler.replace_image(parent, f.context, empty));
    CHECK(f.scheduler.find(parent)->record_count() == 1);
    CHECK(!f.scheduler.current_thread());
    CHECK(!f.scheduler.complete_wait(second_again, WaitCompletion::ready));
    CHECK(!f.scheduler.complete_wait(first_wait, WaitCompletion::timeout));
    const auto after_exec = f.wait(parent);
    CHECK(after_exec.thread == first_tid && after_exec.generation > first_wait.generation);
    CHECK(!f.scheduler.complete_wait(first_wait, WaitCompletion::ready));
    CHECK(f.scheduler.complete_wait(after_exec, WaitCompletion::ready));
    // A directly cloned blocked model process must not inherit a pending wait.
    const auto parent_pending = f.wait(parent);
    const auto clone = f.scheduler.fork_process(parent);
    CHECK(clone);
    CHECK(f.scheduler.find_thread(parent_pending.owner, parent_pending.thread)->wait_registration.pending(parent_pending));
    const auto clone_wait = f.wait(clone.value());
    CHECK(clone_wait.owner != parent_pending.owner && clone_wait.thread != parent_pending.thread);
    CHECK(f.scheduler.complete_wait(clone_wait, WaitCompletion::ready));
    CHECK(f.scheduler.complete_wait(parent_pending, WaitCompletion::ready));
}

void test_pipe_readiness_and_peer_exit()
{
    Fixture f;
    const auto reader = f.create();
    auto pipe = f.scheduler.create_pipe(f.scheduler.find(reader)->resources());
    CHECK(pipe);
    const auto pipe_id = f.scheduler.open_description(f.scheduler.find(reader)->fd_table().entry(pipe.value()[0])->description)->pipe;
    const auto writer_result = f.scheduler.fork_process(reader);
    CHECK(writer_result);
    const auto writer = writer_result.value();
    CHECK(f.scheduler.close_descriptor(f.scheduler.find(reader)->resources(), pipe.value()[1]));
    CHECK(f.scheduler.close_descriptor(f.scheduler.find(writer)->resources(), pipe.value()[0]));
    std::array<std::byte, 1> one{std::byte{0x37}}, out{};
    CHECK(f.scheduler.activate(reader));
    auto read = f.scheduler.read_pipe(pipe_id, out);
    CHECK(!read && read.status().code() == StatusCode::would_block);
    const auto ticket = f.scheduler.current_thread()->wait_registration.ticket();
    CHECK(f.scheduler.current_thread()->wait_registration.pending(ticket));
    f.scheduler.wake_waiters(ThreadWaitReason::pipe_write, pipe_id);
    f.scheduler.wake_waiters(ThreadWaitReason::pipe_read, pipe_id + 1);
    CHECK(f.scheduler.current_thread()->wait_registration.pending(ticket));
    CHECK(f.scheduler.activate(writer));
    CHECK(f.scheduler.write_pipe(pipe_id, one).value() == 1);
    CHECK(!f.scheduler.complete_wait(ticket, WaitCompletion::timeout));
    CHECK(f.scheduler.find_thread(ticket.owner, ticket.thread)->state == ProcessState::runnable);
    CHECK(f.scheduler.activate(reader));
    CHECK(f.scheduler.read_pipe(pipe_id, out).value() == 1 && out == one);
    CHECK(!f.scheduler.read_pipe(pipe_id, out));
    const auto eof_wait = f.scheduler.current_thread()->wait_registration.ticket();
    CHECK(f.scheduler.exit_process(writer, 0));
    CHECK(!f.scheduler.complete_wait(eof_wait, WaitCompletion::timeout));
    CHECK(f.scheduler.find_thread(eof_wait.owner, eof_wait.thread)->state == ProcessState::runnable);
    CHECK(f.scheduler.activate(reader));
    CHECK(f.scheduler.read_pipe(pipe_id, out).value() == 0);
}

void test_pipe_writer_peer_exit()
{
    Fixture f;
    const auto reader = f.create();
    auto pipe = f.scheduler.create_pipe(f.scheduler.find(reader)->resources());
    CHECK(pipe);
    const auto pipe_id = f.scheduler.open_description(f.scheduler.find(reader)->fd_table().entry(pipe.value()[0])->description)->pipe;
    auto writer_result = f.scheduler.fork_process(reader);
    CHECK(writer_result);
    const auto writer = writer_result.value();
    CHECK(f.scheduler.close_descriptor(f.scheduler.find(reader)->resources(), pipe.value()[1]));
    CHECK(f.scheduler.close_descriptor(f.scheduler.find(writer)->resources(), pipe.value()[0]));
    CHECK(f.scheduler.activate(writer));
    std::array<std::byte, pipe_capacity> fill{};
    CHECK(f.scheduler.write_pipe(pipe_id, fill).value() == fill.size());
    CHECK(!f.scheduler.write_pipe(pipe_id, std::span<const std::byte>{fill.data(), 1}));
    const auto wait = f.scheduler.current_thread()->wait_registration.ticket();
    CHECK(f.scheduler.current_thread()->wait_registration.pending(wait));
    CHECK(f.scheduler.exit_process(reader, 0));
    CHECK(!f.scheduler.complete_wait(wait, WaitCompletion::timeout));
    CHECK(f.scheduler.find_thread(wait.owner, wait.thread)->state == ProcessState::runnable);
    CHECK(f.scheduler.activate(writer));
    auto broken = f.scheduler.write_pipe(pipe_id, std::span<const std::byte>{fill.data(), 1});
    CHECK(!broken && broken.status().code() == StatusCode::broken_pipe);
}

void test_child_exit_wake()
{
    Fixture f;
    const auto parent = f.create();
    CHECK(f.scheduler.activate(parent));
    auto child = f.scheduler.fork_process(parent);
    CHECK(child);
    auto pending = f.scheduler.wait_process(parent, child.value());
    CHECK(!pending && pending.status().code() == StatusCode::would_block);
    const auto ticket = f.scheduler.current_thread()->wait_registration.ticket();
    CHECK(f.scheduler.current_thread()->wait_registration.pending(ticket));
    CHECK(f.scheduler.exit_process(child.value(), 23));
    CHECK(!f.scheduler.complete_wait(ticket, WaitCompletion::timeout));
    CHECK(f.scheduler.find_thread(ticket.owner, ticket.thread)->state == ProcessState::runnable);
    CHECK(f.scheduler.activate(parent));
    const auto result = f.scheduler.wait_process(parent, child.value());
    CHECK(result && result.value() == 23);
    CHECK(!f.scheduler.find(child.value()));
    CHECK(!f.scheduler.complete_wait(ticket, WaitCompletion::ready));
}

void test_finite_wait_capacity()
{
    Fixture f;
    constexpr usize capacity = max_processes * max_threads_per_process;
    std::array<WaitTicket, capacity> tickets{};
    usize count = 0;
    for (usize process = 0; process < max_processes; ++process)
    {
        const auto pid = f.create();
        for (usize thread = 1; thread < max_threads_per_process; ++thread)
            CHECK(f.scheduler.create_thread(pid, f.context));
        CHECK(!f.scheduler.create_thread(pid, f.context));
        for (usize thread = 0; thread < max_threads_per_process; ++thread)
        {
            tickets[count] = f.wait(pid, ThreadWaitReason::pipe_read, 101);
            for (usize old = count - thread; old < count; ++old)
                CHECK(tickets[old].thread != tickets[count].thread);
            ++count;
        }
        CHECK(f.scheduler.find(pid)->state() == ProcessState::blocked);
    }
    CHECK(count == capacity && f.scheduler.process_count() == max_processes);
    CHECK(!f.scheduler.create_process("overflow", f.context));
    CHECK(!f.scheduler.schedule_next());
    // Every wait is linked on the channel while the entry pool lasts (256
    // entries on the UP profiles); the rest count as unqueued and retry on a
    // deadline, so each wait still completes.
    const usize linked = f.scheduler.wait_channels().linked(static_cast<u8>(ThreadWaitReason::pipe_read), 101);
    CHECK(linked == (capacity < wait_hash_entries ? capacity : wait_hash_entries));
    CHECK(f.scheduler.unqueued_waits() == capacity - linked);
    f.scheduler.wake_waiters(ThreadWaitReason::pipe_read, 101);
    CHECK(f.scheduler.wait_channels().linked(static_cast<u8>(ThreadWaitReason::pipe_read), 101) == 0);
    for (usize pass = 0; pass < capacity && f.scheduler.earliest_deadline() != DeadlineHeap<deadline_keys>::never; ++pass)
    {
        std::array<u16, max_processes> due{};
        CHECK(f.scheduler.expire_deadlines(1'000'000'000, due) == 0);
    }
    CHECK(f.scheduler.earliest_deadline() == DeadlineHeap<deadline_keys>::never);
    for (const auto &ticket : tickets)
    {
        auto *thread = f.scheduler.find_thread(ticket.owner, ticket.thread);
        CHECK(thread && thread->state == ProcessState::runnable);
        CHECK(!thread->wait_registration.pending(ticket));
        CHECK(!f.scheduler.complete_wait(ticket, WaitCompletion::timeout));
    }
    CHECK(f.scheduler.schedule_next());
}

void test_signal_termination_range()
{
    Fixture f;
    const auto parent = f.create();
    for (unsigned signal = 1; signal <= 128; ++signal)
    {
        CHECK(f.scheduler.activate(parent));
        const auto child = f.scheduler.fork_process(parent);
        CHECK(child);
        const auto ticket = f.wait(parent, ThreadWaitReason::child, child.value());
        CHECK(f.scheduler.terminate_process(child.value(), static_cast<u8>(signal)));
        const auto *dead = f.scheduler.find(child.value());
        CHECK(dead && dead->state() == ProcessState::zombie);
        // Preserve the raw Linux-compatible word; 128 aliases the core bit.
        CHECK(dead->wait_status() == static_cast<i32>(signal));
        CHECK(dead->leader().state == ProcessState::exited);
        CHECK(!f.scheduler.complete_wait(ticket, WaitCompletion::timeout));
        CHECK(f.scheduler.find(parent)->leader().state == ProcessState::runnable);
        CHECK(!f.scheduler.terminate_process(child.value(), static_cast<u8>(signal)));
        CHECK(f.scheduler.wait_process(parent, child.value()));
        CHECK(!f.scheduler.find(child.value()));
    }
    for (const u8 signal : {u8{0}, u8{129}, u8{255}})
    {
        const auto child = f.scheduler.fork_process(parent);
        CHECK(child);
        CHECK(!f.scheduler.terminate_process(child.value(), signal));
        CHECK(f.scheduler.find(child.value())->state() == ProcessState::runnable);
        CHECK(f.scheduler.exit_process(child.value(), 7));
        CHECK(f.scheduler.wait_process(parent, child.value()));
    }
}

// The queue path (smp-scaling WP4a, spec 4.1 and 4.2) on one model CPU
// and on two: transitions queue user threads, pick validates entries, and
// the process state follows its threads.
struct QueueFixture
{
    std::unique_ptr<Scheduler> owned{std::make_unique<Scheduler>()};
    Scheduler &scheduler{*owned};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(0x1000, 0x8000)};
    explicit QueueFixture(usize cpus = 1)
    {
        CHECK(scheduler.configure_cpus(cpus));
        CHECK(scheduler.set_online_cpus((1u << cpus) - 1));
    }
    ProcessId user()
    {
        auto pid = scheduler.create_user_process("queued-user", context);
        CHECK(pid);
        scheduler.find_thread(pid.value(), pid.value())->native_active = true;
        CHECK(scheduler.set_runnable(pid.value()));
        return pid.value();
    }
    ThreadControlBlock &thread(ProcessId pid, ThreadId tid = 0) { return *scheduler.find_thread(pid, tid ? tid : pid); }
    // Picks on `cpu` and checks the identity.
    void run(ProcessId pid, ThreadId tid = 0, smp::CpuId cpu = 0)
    {
        const auto picked = scheduler.pick(cpu);
        CHECK(picked && picked->ref == (ThreadRef{pid, tid ? tid : pid}) && !picked->probed);
        CHECK(thread(pid, tid).state == ProcessState::running && thread(pid, tid).on_cpu == cpu);
    }
};

void test_queue_path_transitions()
{
    QueueFixture f;
    const auto pid = f.user();
    // Created and made runnable: queued on the only CPU.
    CHECK(f.thread(pid).queued_cpu == 0 && f.scheduler.run_queue(0).size() == 1);
    CHECK(f.scheduler.find(pid)->state() == ProcessState::runnable);
    f.run(pid);
    CHECK(f.scheduler.find(pid)->state() == ProcessState::running && f.scheduler.run_queue(0).empty());
    // Armed, then completed while still on the CPU: put_prev queues it.
    auto ticket = f.scheduler.register_current_wait(ThreadWaitReason::poll, 3);
    CHECK(ticket && f.scheduler.find(pid)->state() == ProcessState::blocked);
    CHECK(f.scheduler.complete_wait(ticket.value(), WaitCompletion::ready));
    CHECK(f.thread(pid).queued_cpu == no_cpu && f.thread(pid).on_cpu == 0);
    CHECK(f.scheduler.find(pid)->state() == ProcessState::runnable);
    f.scheduler.put_prev(0);
    CHECK(f.thread(pid).queued_cpu == 0 && f.thread(pid).on_cpu == no_cpu);
    CHECK(f.thread(pid).state == ProcessState::runnable && !f.scheduler.cpu_slot(0).occupied());
    // Parked, then completed: the completion queues it.
    f.run(pid);
    ticket = f.scheduler.register_current_wait(ThreadWaitReason::poll, 4);
    CHECK(ticket);
    f.scheduler.put_prev(0);
    CHECK(f.thread(pid).queued_cpu == no_cpu && f.scheduler.run_queue(0).empty());
    CHECK(f.scheduler.find(pid)->state() == ProcessState::blocked);
    CHECK(f.scheduler.complete_wait(ticket.value(), WaitCompletion::timeout));
    CHECK(f.thread(pid).queued_cpu == 0 && f.scheduler.run_queue(0).size() == 1);
    CHECK(f.scheduler.wakes(WakeSource::timer) == 1 && f.scheduler.wakes(WakeSource::thread) == 1);
    // A clone is queued at creation, after the queued leader (FIFO).
    auto sibling = f.scheduler.create_thread_prepared(pid, f.context, [](ThreadControlBlock &created) {
        created.native_active = true;
        return Status::success();
    });
    CHECK(sibling && f.thread(pid, sibling.value()).queued_cpu == 0 && f.scheduler.run_queue(0).size() == 2);
    f.run(pid);
    f.scheduler.put_prev(0);
    f.run(pid, sibling.value());
    f.scheduler.put_prev(0);
    f.run(pid);
    f.scheduler.put_prev(0);
    // A stopped process's entries are dropped at pick; continue queues them again.
    CHECK(f.scheduler.stop_process(pid, 19));
    CHECK(!f.scheduler.pick(0) && f.scheduler.run_queue(0).empty());
    CHECK(f.thread(pid).queued_cpu == no_cpu && f.thread(pid, sibling.value()).queued_cpu == no_cpu);
    CHECK(f.scheduler.continue_process(pid));
    CHECK(f.scheduler.run_queue(0).size() == 2);
    // A parked thread whose wait a signal would interrupt: probed, picked
    // blocked, and runnable only once its resume completes the wait.
    f.run(pid);
    ticket = f.scheduler.register_current_wait(ThreadWaitReason::pipe_read, 5);
    CHECK(ticket);
    f.scheduler.put_prev(0);
    CHECK(f.scheduler.probe_parked(*f.scheduler.find(pid), f.thread(pid)));
    CHECK(!f.scheduler.probe_parked(*f.scheduler.find(pid), f.thread(pid)));
    f.run(pid, sibling.value());
    f.scheduler.put_prev(0);
    const auto probed = f.scheduler.pick(0);
    CHECK(probed && probed->ref == (ThreadRef{pid, pid}) && probed->probed);
    CHECK(f.thread(pid).state == ProcessState::blocked && !f.thread(pid).probe && f.thread(pid).on_cpu == 0);
    CHECK(!f.scheduler.resume_current(0));
    CHECK(f.scheduler.complete_wait(ticket.value(), WaitCompletion::interrupted));
    CHECK(f.scheduler.resume_current(0) && f.thread(pid).state == ProcessState::running);
    f.scheduler.put_prev(0);
    // A kernel_thread record is never queued by a transition.
    auto kernel = f.scheduler.create_process("kernel-record", f.context);
    CHECK(kernel && f.scheduler.set_runnable(kernel.value()));
    CHECK(f.thread(kernel.value()).queued_cpu == no_cpu);
    // Nothing for the audit to repair; an orphaned runnable thread is repaired once.
    CHECK(f.scheduler.audit_queues() == 0);
    // A missed enqueue (the leader made runnable behind the scheduler's back)
    // and a blocked thread without a pending wait: one repair each.
    CHECK(f.scheduler.block_process(pid) && f.scheduler.run_queue(0).empty());
    f.thread(pid).state = ProcessState::runnable;
    const auto repairs = smp::check_total(smp::KernelCheck::audit_repairs);
    CHECK(f.scheduler.audit_queues() == 2 && f.thread(pid).queued_cpu == 0);
    CHECK(f.thread(pid, sibling.value()).state == ProcessState::runnable && f.thread(pid, sibling.value()).queued_cpu == 0);
    CHECK(smp::check_total(smp::KernelCheck::audit_repairs) == repairs + 2 && f.scheduler.audit_queues() == 0);
    CHECK(smp::check_total(smp::KernelCheck::queue) == 0 && smp::check_total(smp::KernelCheck::pin) == 0);
}

void test_queue_path_two_cpus()
{
    QueueFixture f{2};
    const auto pid = f.user();
    CHECK(f.thread(pid).queued_cpu == 0);
    // CPU 1 steals from CPU 0's queue; the CPU slot pins the process.
    {
        const smp::ScopedHostedCpu cpu{1};
        f.run(pid, 0, 1);
        CHECK(f.scheduler.cpu_slot(1).occupied() && f.scheduler.pin(pid));
        f.scheduler.put_prev(1);
    }
    CHECK(f.thread(pid).queued_cpu == 1 && f.thread(pid).last_cpu == 1);
    // An offline CPU's queue is closed and its entries move.
    CHECK(f.scheduler.offline_cpu(1));
    CHECK(f.scheduler.run_queue(1).closed() && f.thread(pid).queued_cpu == 0);
    CHECK(f.scheduler.online_cpu(1) && !f.scheduler.run_queue(1).closed());
    // An entry the affinity no longer admits is queued again where it may run.
    CHECK(f.scheduler.set_thread_affinity(*f.scheduler.find(pid), f.thread(pid), 1u << 1));
    CHECK(!f.scheduler.pick(0) && f.thread(pid).queued_cpu == 1);
    // The idle handshake: an empty system halts; the waker's push wakes it.
    // Without concurrent locks (a UP build) only the own queue counts and no
    // CPU is ever woken by another.
    constexpr bool concurrent = smp::concurrent_locks;
    CHECK(f.scheduler.enter_idle(0) ==
          (concurrent ? SchedulerState::IdleCheck::steal_hint : SchedulerState::IdleCheck::halt));
    {
        const smp::ScopedHostedCpu cpu{1};
        f.run(pid, 0, 1);
        auto ticket = f.scheduler.register_current_wait(ThreadWaitReason::poll, 9);
        CHECK(ticket);
        f.scheduler.put_prev(1);
        CHECK(f.scheduler.enter_idle(1) == SchedulerState::IdleCheck::halt && f.scheduler.cpu_idle(1));
        CHECK(!f.scheduler.take_wake_request(1));
        {
            const smp::ScopedHostedCpu waker{0};
            CHECK(f.scheduler.complete_wait(ticket.value(), WaitCompletion::ready));
        }
        CHECK(f.scheduler.take_wake_request(1) == concurrent && !f.scheduler.take_wake_request(1));
        CHECK(f.scheduler.enter_idle(1) == SchedulerState::IdleCheck::own_work && !f.scheduler.cpu_idle(1));
        f.run(pid, 0, 1);
        f.scheduler.put_prev(1);
    }
    // A reaped process keeps its slot while a pin holds it; the CPU slot's
    // own pin went with the exit's put_prev of CPU 1.
    {
        const smp::ScopedHostedCpu cpu{1};
        f.run(pid, 0, 1);
    }
    auto held = f.scheduler.pin(pid);
    CHECK(held && f.scheduler.exit_process(pid, 0) && !f.scheduler.cpu_slot(1).occupied());
    CHECK(f.scheduler.reap_process(pid) && !f.scheduler.find(pid) && f.scheduler.slot_of(pid));
    held.release();
    CHECK(!f.scheduler.slot_of(pid));
    CHECK(smp::check_total(smp::KernelCheck::queue) == 0 && smp::check_total(smp::KernelCheck::pin) == 0);
}

// Exec from a sibling while the leader is parked, or after it exited: the
// leader carries the new image but is on no CPU, so exec itself queues it.
void test_queue_path_exec_from_sibling()
{
    QueueFixture f;
    const auto user = arch::configured_arch_operations().make_user_context({0x40'0000, 0x80'0000});
    const auto repairs = smp::check_total(smp::KernelCheck::audit_repairs);
    for (const bool leader_exited : {false, true})
    {
        const auto pid = f.user();
        auto sibling = f.scheduler.create_thread_prepared(pid, f.context, [](ThreadControlBlock &created) {
            created.native_active = true;
            return Status::success();
        });
        CHECK(sibling);
        f.run(pid);
        if (leader_exited)
            CHECK(f.scheduler.exit_thread(pid, pid, 0));
        else
        {
            CHECK(f.scheduler.register_current_wait(ThreadWaitReason::futex, 6));
            f.scheduler.put_prev(0);
        }
        CHECK(f.thread(pid).on_cpu == no_cpu && f.thread(pid).queued_cpu == no_cpu);
        f.run(pid, sibling.value());
        CHECK(f.scheduler.replace_image(pid, user, {}));
        CHECK(f.scheduler.find(pid)->record_count() == 1 && !f.scheduler.cpu_slot(0).occupied());
        CHECK(f.thread(pid).state == ProcessState::runnable && f.thread(pid).queued_cpu == 0);
        CHECK(f.scheduler.run_queue(0).size() == 1 && f.scheduler.audit_queues() == 0);
        // The loader's install gives the new image its native context.
        f.thread(pid).native_active = true;
        f.run(pid);
        f.scheduler.put_prev(0);
        CHECK(f.scheduler.exit_process(pid, 0) && f.scheduler.reap_process(pid));
    }
    CHECK(smp::check_total(smp::KernelCheck::queue) == 0 && smp::check_total(smp::KernelCheck::pin) == 0);
    CHECK(smp::check_total(smp::KernelCheck::audit_repairs) == repairs);
}

int main()
{
    test_queue_path_transitions();
    test_queue_path_two_cpus();
    test_queue_path_exec_from_sibling();
    test_signal_termination_range();
    test_identity_rearm_and_completion_order();
    test_stable_slots_and_lifecycle();
    test_current_thread_exec_and_fork();
    test_pipe_readiness_and_peer_exit();
    test_pipe_writer_peer_exit();
    test_child_exit_wake();
    test_finite_wait_capacity();
    std::printf("PASS scheduler wait integration checks=%zu guest=0 smp=0\n", checks);
}
