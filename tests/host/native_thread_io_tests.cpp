// Exercise real marshalling and scheduler waits with hosted copies only.
#include "native_clock_model.hpp"

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    auto created = kernel.scheduler().create_user_process("peer-io", kernel.arch().make_user_context(
        {hw::user_begin, hw::user_end - hw::page_size}));
    CHECK(created);
    const auto pid = created.value();
    auto *process = kernel.scheduler().find(pid);
    const auto first = process->threads()[0].tid;
    auto second = kernel.scheduler().create_thread(pid, process->threads()[0].context);
    CHECK(second && kernel.scheduler().set_runnable(pid));
    CHECK(kernel.posix().activate(pid));
    auto descriptors = kernel.posix().pipe(0);
    CHECK(descriptors);
    auto activate = [&] { CHECK(kernel.scheduler().activate_thread(pid, second.value())); };
    auto *peer = kernel.scheduler().find_thread(pid, second.value());
    auto *leader = kernel.scheduler().find_thread(pid, first);
    leader->wait_key = 73;
    std::array<uptr, 2> vector{hw::user_begin + 128, 1};
    std::memcpy(mock::memory.data(), vector.data(), sizeof(vector));
    for (auto number : {OK_SYS_READ, OK_SYS_READV})
    {
        activate();
        bool complete = true;
        auto result = native::dispatch(kernel, pid, {number,
            {static_cast<uptr>(descriptors.value()[0]), hw::user_begin, 1, 0, 0, 0}}, complete);
        CHECK(result == -11 && !complete);
        CHECK(peer->state == sched::ProcessState::blocked && peer->wait_registration.active());
        CHECK(leader->state == sched::ProcessState::runnable && leader->wait_key == 73);
        CHECK(kernel.scheduler().complete_wait(peer->wait_registration.ticket(), sched::WaitCompletion::cancelled));
        // The resumed continuation reports the cancellation before the next call.
        activate();
        CHECK(native::dispatch(kernel, pid, {number,
            {static_cast<uptr>(descriptors.value()[0]), hw::user_begin, 1, 0, 0, 0}}, complete) == -125);
        CHECK(!peer->wait_registration.active());
    }
    // A full pipe must retain the selected writer continuation as well.
    std::array<std::byte, sched::pipe_capacity> data{};
    const auto *writer = kernel.scheduler().open_description(process->fd_table().entry(descriptors.value()[1])->description);
    CHECK(writer);
    CHECK(kernel.scheduler().write_pipe(writer->pipe, data, true).value() == data.size());
    for (auto number : {OK_SYS_WRITE, OK_SYS_WRITEV})
    {
        activate();
        bool complete = true;
        CHECK(native::dispatch(kernel, pid, {number,
            {static_cast<uptr>(descriptors.value()[1]), hw::user_begin, 1, 0, 0, 0}}, complete) == -11);
        CHECK(!complete && peer->wait_registration.active() && peer->state == sched::ProcessState::blocked);
        CHECK(leader->state == sched::ProcessState::runnable && leader->wait_key == 73);
        CHECK(kernel.scheduler().complete_wait(peer->wait_registration.ticket(), sched::WaitCompletion::cancelled));
        activate();
        CHECK(native::dispatch(kernel, pid, {number,
            {static_cast<uptr>(descriptors.value()[1]), hw::user_begin, 1, 0, 0, 0}}, complete) == -125);
    }
    CHECK(kernel.scheduler().read_pipe(writer->pipe, data, true).value() == data.size());
    activate();
    bool rejected = true;
    CHECK(native::dispatch(kernel, pid + 999, {OK_SYS_READ, {}}, rejected) == -22 && rejected);
    auto child1 = kernel.scheduler().fork_process(pid);
    auto child2 = kernel.scheduler().fork_process(pid);
    CHECK(child1 && child2);
    activate();
    bool complete = true;
    CHECK(native::dispatch(kernel, pid, {OK_SYS_WAIT4, {~uptr{0}, 0, 0, 0, 0, 0}}, complete) == -11);
    CHECK(!complete && peer->wait_registration.active() && peer->wait_key == 0);
    CHECK(leader->wait_key == 73 && !leader->wait_registration.active());
    CHECK(kernel.scheduler().exit_process(child2.value(), 7));
    CHECK(!peer->wait_registration.active() && peer->state == sched::ProcessState::runnable);
    activate();
    complete = true;
    CHECK(native::dispatch(kernel, pid, {OK_SYS_WAIT4, {~uptr{0}, 0, 0, 0, 0, 0}}, complete) == static_cast<i64>(child2.value()));
    CHECK(complete);
    CHECK(kernel.scheduler().exit_process(child1.value(), 0));
    activate();
    CHECK(native::dispatch(kernel, pid, {OK_SYS_WAIT4, {~uptr{0}, 0, 0, 0, 0, 0}}, complete) == static_cast<i64>(child1.value()));
    CHECK(kernel.scheduler().terminate_process(pid, 9));
    std::puts("PASS hosted selected-thread scalar/vector pipe and any-child wait completion; no pthread/guest/SMP claim");
}
