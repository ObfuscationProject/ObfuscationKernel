// Actual runtime continuations with hosted copy, clock and trap backends.
#include "native_clock_model.hpp"

using sched::WaitCompletion;
using sched::ThreadWaitReason;

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    auto created = kernel.scheduler().create_user_process("wait-owner", kernel.arch().make_user_context(
        {hw::user_begin, hw::user_end - 4096}));
    CHECK(created);
    const auto pid = created.value();
    CHECK(kernel.posix().activate(pid));
    auto *process = kernel.scheduler().find(pid);
    auto &thread = process->threads()[0];
    CHECK(native::prepare_native_thread(thread));
    thread.native_active = true;
    native::started = true;
    auto pipes = kernel.posix().pipe(posix::o_NONBLOCK);
    CHECK(pipes);
    const auto reader = process->fd_table().entry(pipes.value()[0])->description;
    const auto writer = process->fd_table().entry(pipes.value()[1])->description;
    std::array<ok_pollfd, 1> fds{{{pipes.value()[0], OK_POLLIN, 0}}};
    native::PollRequest request{};
    request.descriptors = fds;
    request.output = hw::user_begin;
    request.timed = true;
    request.timeout = {0, 50};
    bool complete = true;
    CHECK(native::begin_poll(kernel, pid, request, complete) == 0 && !complete);
    const auto first = thread.wait_registration.ticket();
    CHECK(thread.wait_registration.pending(first));
    CHECK(kernel.scheduler().open_description(reader)->references == 2);

    // A cancelled registration's delayed runtime cleanup must not complete a
    // replacement sleep, change its frame or retain the old description pin.
    CHECK(kernel.scheduler().complete_wait(first, WaitCompletion::cancelled));
    CHECK(kernel.posix().activate(pid));
    thread.native_frame.registers[0] = 0xabc;
    CHECK(native::begin_sleep(kernel, pid, mock::now, {0, 100}, false, complete));
    const auto replacement = thread.wait_registration.ticket();
    CHECK(replacement.generation != first.generation);
    const auto outputs = mock::outputs;
    CHECK(native::step(kernel));
    CHECK(mock::outputs == outputs && thread.native_frame.registers[0] == 0xabc);
    CHECK(kernel.scheduler().open_description(reader)->references == 1);
    CHECK(thread.wait_registration.pending(replacement) && thread.native_deadline.armed());
    CHECK(!kernel.scheduler().complete_wait(first, WaitCompletion::ready));
    mock::now += 100;
    CHECK(native::step(kernel));
    CHECK(thread.wait_registration.completion() == WaitCompletion::timeout);
    CHECK(thread.native_frame.registers[0] == 0 && !thread.native_deadline.armed());
    CHECK(!kernel.scheduler().complete_wait(replacement, WaitCompletion::ready));

    // Data and timeout become ready in the same serialized step: one result,
    // readiness wins, and a later step cannot copy the completion twice.
    CHECK(kernel.posix().activate(pid));
    complete = true;
    CHECK(native::begin_poll(kernel, pid, request, complete) == 0 && !complete);
    const auto simultaneous = thread.wait_registration.ticket();
    const std::byte byte{42};
    CHECK(kernel.scheduler().write_pipe(kernel.scheduler().open_description(writer)->pipe, {&byte, 1}, true));
    mock::now += 50;
    const auto before = mock::outputs;
    CHECK(native::step(kernel));
    CHECK(mock::outputs == before + 1 && thread.native_frame.registers[0] == 1);
    CHECK(thread.wait_registration.completion() == WaitCompletion::ready);
    CHECK(!kernel.scheduler().complete_wait(simultaneous, WaitCompletion::timeout));
    CHECK(native::step(kernel) && mock::outputs == before + 1);
    CHECK(kernel.scheduler().open_description(reader)->references == 1);

    // Runtime cleanup finds the exact thread, even when it is not threads()[0].
    std::array<std::byte, 1> consumed{};
    CHECK(kernel.scheduler().read_pipe(kernel.scheduler().open_description(reader)->pipe, consumed, true));
    auto second = kernel.scheduler().create_thread_prepared(pid, thread.context,
        [](sched::ThreadControlBlock &created) { return native::prepare_native_thread(created); });
    CHECK(second);
    CHECK(kernel.posix().activate(pid));
    CHECK(kernel.scheduler().current_tid(0) == thread.tid);
    CHECK(kernel.scheduler().block_current(ThreadWaitReason::console_read, 99));
    CHECK(kernel.posix().activate(pid));
    CHECK(kernel.scheduler().current_tid(0) == second.value());
    auto *peer = kernel.scheduler().find_thread(pid, second.value());
    CHECK(peer);
    thread.native_active = false;
    peer->native_active = true;
    peer->native_frame.pc = hw::user_begin + 256;
    thread.native_frame.pc = hw::user_begin + 128;
    peer->native_frame.registers[0] = 0xdef;
    thread.native_frame.registers[0] = 0x123;
    complete = true;
    CHECK(native::begin_poll(kernel, pid, request, complete) == 0 && !complete);
    const auto peer_ticket = peer->wait_registration.ticket();
    CHECK(peer_ticket.thread == second.value() && !thread.wait_registration.pending(peer_ticket));
    mock::now += 50;
    const auto entries = mock::entries;
    CHECK(native::step(kernel));
    CHECK(mock::entries == entries + 1 && mock::last_frame.pc == hw::user_begin + 256);
    CHECK(peer->wait_registration.completion() == WaitCompletion::timeout);
    CHECK(peer->native_frame.registers[0] == 0 && thread.native_frame.registers[0] == 0x123);
    CHECK(kernel.scheduler().open_description(reader)->references == 1);
    CHECK(kernel.scheduler().complete_wait(thread.wait_registration.ticket(), WaitCompletion::ready));
    thread.native_active = true;
    CHECK(native::step(kernel) && mock::last_frame.pc == hw::user_begin + 128);
    CHECK(native::step(kernel) && mock::last_frame.pc == hw::user_begin + 256);
    CHECK(native::step(kernel) && mock::last_frame.pc == hw::user_begin + 128);
    native::release_process(kernel, pid);
    CHECK(native::entry_stacks.accounting().leased_slots == 0 && native::runtime_status());
    CHECK(kernel.scheduler().terminate_process(pid, 9));
    CHECK(!kernel.scheduler().complete_wait(peer_ticket, WaitCompletion::ready));
    std::puts("PASS hosted runtime wait ownership: stale cleanup, single completion, ready/timeout ordering, exact thread and teardown");
}
