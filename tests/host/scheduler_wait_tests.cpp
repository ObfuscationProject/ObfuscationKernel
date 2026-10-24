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
    forged.thread = f.scheduler.find(other)->threads()[0].tid;
    CHECK(!f.scheduler.complete_wait(forged, WaitCompletion::ready));
    forged = ticket;
    ++forged.generation;
    CHECK(!f.scheduler.complete_wait(forged, WaitCompletion::timeout));
    CHECK(thread->wait_registration.pending(ticket));
    CHECK(thread->native_deadline.arm(10, 0, 20, false));
    CHECK(f.scheduler.complete_wait(ticket, WaitCompletion::ready));
    CHECK(!thread->native_deadline.armed());
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

void test_compaction_and_lifecycle()
{
    Fixture f;
    const auto before = f.create(), owner = f.create(), after = f.create();
    const auto ticket = f.wait(owner);
    const auto old_address = reinterpret_cast<uptr>(f.scheduler.find(owner));
    CHECK(f.scheduler.exit_process(before, 0));
    CHECK(f.scheduler.reap_process(before));
    CHECK(f.scheduler.find(owner) && f.scheduler.find(after));
    CHECK(reinterpret_cast<uptr>(f.scheduler.find(owner)) != old_address);
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
    const auto first_tid = f.scheduler.find(parent)->threads()[0].tid;
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
    auto &child_thread = f.scheduler.find(child.value())->threads()[0];
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
    CHECK(f.scheduler.find(parent)->threads().size() == 1);
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
            tickets[count] = f.wait(pid, ThreadWaitReason::poll, 101);
            for (usize old = count - thread; old < count; ++old)
                CHECK(tickets[old].thread != tickets[count].thread);
            ++count;
        }
        CHECK(f.scheduler.find(pid)->state() == ProcessState::blocked);
    }
    CHECK(count == capacity && f.scheduler.process_count() == max_processes);
    CHECK(!f.scheduler.create_process("overflow", f.context));
    CHECK(!f.scheduler.schedule_next());
    f.scheduler.wake_waiters(ThreadWaitReason::poll, 101);
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
        CHECK(dead->threads()[0].state == ProcessState::exited);
        CHECK(!f.scheduler.complete_wait(ticket, WaitCompletion::timeout));
        CHECK(f.scheduler.find(parent)->threads()[0].state == ProcessState::runnable);
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

int main()
{
    test_signal_termination_range();
    test_identity_rearm_and_completion_order();
    test_compaction_and_lifecycle();
    test_current_thread_exec_and_fork();
    test_pipe_readiness_and_peer_exit();
    test_pipe_writer_peer_exit();
    test_child_exit_wake();
    test_finite_wait_capacity();
    std::printf("PASS scheduler wait integration checks=%zu guest=0 smp=0\n", checks);
}
