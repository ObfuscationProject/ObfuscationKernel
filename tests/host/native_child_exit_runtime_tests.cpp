// Actual private runtime with hosted trap, mapping and copy backends.
#define OK_ENABLE_TEST_POINTS 1
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_child_exit_runtime_fixture.hpp"

namespace {
void actual_matrix(Kernel &kernel)
{
    for (u64 disposition : {u64{0}, u64{1}, handler})
        for (bool blocked : {false, true}) for (bool no_wait : {false, true})
            for (bool killed : {false, true})
            {
                const auto parent = create(kernel);
                action(kernel, parent, disposition, no_wait ? Actions::no_child_wait : 0);
                kernel.scheduler().find_thread(parent, parent)->native_signal_mask.set(sigchld, blocked);
                const auto child = fork(kernel, parent);
                const auto *borrowed_parent = kernel.scheduler().find(parent), *borrowed_child = kernel.scheduler().find(child);
                if (killed)
                {
                    expect(native::private_process_kill(kernel, *kernel.scheduler().find(parent), child, 9) == 0);
                    native::private_process_signal_fatal(kernel);
                }
                else raw_exit(kernel, child, child, 23, true);
                expect(native::runtime_status().ok());
                expect(kernel.scheduler().find(parent) == borrowed_parent && kernel.scheduler().find(child) == borrowed_child);
                expect(!borrowed_child->resources().native_space.root && borrowed_child->resources().native_pages.empty());
                expect(borrowed_child->threads()[0].entry_stack.pool == nullptr);
                const bool notified = disposition != 1 && (disposition > 1 || blocked);
                const bool autoreap = disposition == 1 || no_wait;
                expect(native::private_signal_find(parent)->shared.pending_mask().contains(sigchld) == notified);
                if (notified)
                {
                    const auto info = pending(parent).info;
                    expect(info.sender_pid == child && info.code == (killed ? 2 : 1) && info.child_status == (killed ? 9 : 23));
                }
                expect(borrowed_child->parent_pid() == (autoreap ? 0 : parent));
                expect(native::private_child_reap(kernel));
                if (!autoreap) CHECK(kernel.scheduler().wait_process(parent, child));
                expect(!kernel.scheduler().find(child)); release(kernel, parent);
            }
}
void actual_threads(Kernel &kernel)
{
    for (bool group : {false, true})
    {
        const auto parent = create(kernel); action(kernel, parent, handler);
        const auto child = fork(kernel, parent), last = peer(kernel, child);
        raw_exit(kernel, child, child, 17);
        expect(!native::private_signal_find(parent)->shared.pending_mask().contains(sigchld));
        expect(kernel.scheduler().find(child)->state() != sched::ProcessState::zombie);
        raw_exit(kernel, child, last, 23, group);
        expect(pending(parent).info.child_status == 17);
        expect(kernel.scheduler().find(child)->wait_status() == 23 * 256);
        CHECK(kernel.scheduler().wait_process(parent, child)); release(kernel, parent);
    }
    const auto parent = create(kernel), creator = peer(kernel, parent);
    kernel.scheduler().find_thread(parent, creator)->native_signal_mask.set(sigchld);
    const auto child = fork(kernel, parent, creator);
    expect(native::private_signal_find(child)->lineage.parent_tid == creator);
    raw_exit(kernel, parent, creator, 17);
    expect(native::private_signal_find(child)->lineage.parent_tid == parent);
    raw_exit(kernel, child, child, 23);
    expect(!native::private_signal_find(parent)->shared.pending_mask().contains(sigchld));
    CHECK(kernel.scheduler().wait_process(parent, child)); release(kernel, parent);
}
void actual_waits_and_rollback(Kernel &kernel)
{
    const auto parent = create(kernel); action(kernel, parent, handler, Actions::no_child_wait);
    const auto before_processes = kernel.scheduler().process_count(), before_stacks = native::entry_stacks.accounting().leased_slots;
    activate(kernel, parent, parent); mock::spaces_enabled = false; bool complete = true;
    expect(native::dispatch(kernel, parent, {OK_SYS_FORK, {}}, complete) < 0);
    expect(kernel.scheduler().process_count() == before_processes && native::entry_stacks.accounting().leased_slots == before_stacks);
    expect(!native::private_signal_find(parent)->shared.pending_mask().contains(sigchld));
    const auto child = fork(kernel, parent);
    activate(kernel, parent, parent);
    expect(native::child_wait_probe(kernel, parent, {static_cast<i32>(child), info_address, false}) == -11);
    raw_exit(kernel, child, child, 23);
    activate(kernel, parent, parent); complete = true;
    expect(native::child_wait_probe(kernel, parent, {static_cast<i32>(child), info_address, false}) == -10);
    expect(pending(parent).info.sender_pid == child); expect(native::private_child_reap(kernel));
    native::private_signal_find(parent)->shared.discard(sigchld);
    // Active sigtimedwait temporarily removes its wanted mask; real_blocked
    // preserves default-ignored automatic generation on its named parent.
    action(kernel, parent, 0);
    const auto second = fork(kernel, parent);
    auto &thread = *kernel.scheduler().find_thread(parent, parent);
    thread.native_signal_mask.set(sigchld); activate(kernel, parent, parent); thread.syscall_pending = true;
    native::PrivateSignalWaitState::Codec::Request request{};
    request.kind = native::SignalWaitKind::timed; request.wanted.set(sigchld); request.info = info_address;
    thread.native_frame.registers[14] = 128;
    CHECK(native::signal_waits.begin(thread, request, 128, mock::now));
    const auto *record = native::signal_waits.find(thread); CHECK(record);
    const auto wait = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::signal_wait, record->epoch); CHECK(wait);
    CHECK(native::signal_waits.bind(thread, wait.value())); expect(!thread.native_signal_mask.contains(sigchld));
    raw_exit(kernel, second, second, 23); expect(pending(parent).info.sender_pid == second);
    expect(native::private_signal_wait_ready(kernel, *kernel.scheduler().find(parent), thread, mock::now));
    expect(native::private_signal_wait_complete(kernel, *kernel.scheduler().find(parent), thread, mock::now));
    expect(!native::private_signal_find(parent)->shared.pending_mask().contains(sigchld));
    expect(thread.native_signal_mask.contains(sigchld)); CHECK(kernel.scheduler().wait_process(parent, second));
    release(kernel, parent);
}
void native_parent_boundary(Kernel &kernel)
{
    const auto parent = create(kernel);
    kernel.scheduler().find(parent)->resources().personality = sched::UserPersonality::native;
    const auto child = fork(kernel, parent);
    // This is the personality transition committed by successful musl exec.
    kernel.scheduler().find(child)->resources().personality = sched::UserPersonality::linux;
    action(kernel, parent, handler, Actions::no_child_wait);
    raw_exit(kernel, child, child, 23);
    expect(kernel.scheduler().find(child)->parent_pid() == parent);
    expect(!native::private_signal_find(parent)->shared.pending_mask().contains(sigchld));
    CHECK(kernel.scheduler().wait_process(parent, child)); release(kernel, parent);
}
void failclosed_owner_and_thread(Kernel &kernel)
{
    const auto parent = create(kernel); action(kernel, parent, handler);
    auto *owner = native::private_signal_find(parent); const auto epoch = owner->epoch;
    CHECK(owner->shared.enqueue_child_exit(sched::ChildExitStatus::normal(23), 123, 456) == native::PrivateProcessQueue::Enqueue::queued);
    const auto ticket = pending(parent);
    auto &process = *kernel.scheduler().find(parent); auto &thread = process.threads()[0];
    auto *queue = native::private_signal_queue(thread); const auto stack = *native::private_signal_altstack(thread);
    owner->pid = 0;
    native::private_signal_exec(kernel, process, queue, stack);
    expect(!native::runtime_status() && !native::started);
    expect(owner->pid == 0 && owner->epoch == epoch && owner->shared.consume(ticket));
    owner->pid = parent; native::runtime_failure = Status::success(); release(kernel, parent);
    const auto next = create(kernel), child = fork(kernel, next);
    activate(kernel, child, child);
    native::futex_waits[0] = {child, child, {&native::futex_objects, ~u32{0}, 0, 1}};
    bool complete = true;
    expect(native::dispatch(kernel, child, {OK_SYS_EXIT, {17}}, complete) < 0 && !complete);
    expect(!native::runtime_status() && !native::started);
    expect(kernel.scheduler().find_thread(child, child)->state != sched::ProcessState::exited);
    expect(native::private_signal_find(child)->lineage.leader_cause == sched::ChildExitStatus{});
    expect(!native::private_signal_find(next)->shared.pending_mask().contains(sigchld));
    const auto entries = mock::entries; expect(!native::step(kernel) && mock::entries == entries);
    native::futex_waits[0] = {}; native::runtime_failure = Status::success();
    release(kernel, child); release(kernel, next);
}
void cleanup_failure(Kernel &kernel)
{
    const auto parent = create(kernel); action(kernel, parent, handler, Actions::no_child_wait);
    const auto child = fork(kernel, parent);
    mock::fail_destroy_root = kernel.scheduler().find(child)->resources().native_space.root;
    native::finish(kernel, child, 23);
    expect(!native::runtime_status() && !native::started);
    expect(kernel.scheduler().find(child)->state() != sched::ProcessState::zombie);
    expect(kernel.scheduler().find(child)->parent_pid() == parent);
    expect(!native::private_signal_find(parent)->shared.pending_mask().contains(sigchld));
    const auto entries = mock::entries; expect(!native::step(kernel) && mock::entries == entries);
    native::runtime_failure = Status::success(); release(kernel, child); release(kernel, parent);
}
void adopted_live_child_runtime(Kernel &kernel)
{
    const auto init = create_init(kernel);
    expect(init == sched::user_init_pid);
    activate(kernel, init, init);
    bool complete = true;
    expect(native::dispatch(kernel, init, {OK_SYS_GETPID, {}}, complete) == 1 && complete);
    action(kernel, init, handler);
    const auto service = fork(kernel, init);
    const auto descendant = fork(kernel, service);
    raw_exit(kernel, service, service, 17, true);
    expect(kernel.scheduler().find(descendant)->parent_pid() == init);
    activate(kernel, descendant, descendant);
    complete = true;
    expect(native::dispatch(kernel, descendant, {OK_SYS_GETPPID, {}}, complete) == 1 && complete);
    const auto *lineage = native::private_signal_find(descendant);
    expect(lineage && lineage->lineage.parent == init &&
           lineage->lineage.parent_epoch == native::private_signal_find(init)->epoch);
    raw_exit(kernel, descendant, descendant, 23, true);
    expect(native::runtime_status().ok());
    expect(kernel.scheduler().find(descendant)->parent_pid() == init);
    CHECK(kernel.scheduler().wait_process(init, service));
    CHECK(kernel.scheduler().wait_process(init, descendant));
    expect(kernel.scheduler().find(init) && kernel.scheduler().find(init)->children().empty());
    raw_exit(kernel, init, init, 0, true);
    expect(kernel.scheduler().find(init) &&
           kernel.scheduler().find(init)->state() == sched::ProcessState::zombie);
}
}
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    actual_matrix(kernel); actual_threads(kernel); actual_waits_and_rollback(kernel); native_parent_boundary(kernel); failclosed_owner_and_thread(kernel); cleanup_failure(kernel); adopted_live_child_runtime(kernel);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    std::printf("CHILD_EXIT_RUNTIME_PASS checks=%zu hosted=1 native=0\n", checks);
    return 0;
}
