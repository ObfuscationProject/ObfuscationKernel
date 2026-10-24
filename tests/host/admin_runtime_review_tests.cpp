// Independent hosted audit: actual Kernel API/runtime, mocked hardware only.
#ifndef OK_NATIVE_SIGNAL_RUNTIME
#error The ordinary runtime gate is required.
#endif
#ifdef OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE
#error Private fixture enablement cannot qualify this ordinary runtime audit.
#endif
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_clock_model.hpp"
#include "../../src/user/native_lifecycle.hpp"
#include <source_location>

namespace {
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "ADMIN_REVIEW_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr uptr site = hw::user_begin + 1024, handler = hw::user_begin + 2048, output = hw::user_begin + 512;
using Actions = native::PrivateChildExit::Actions;
constexpr auto sigchld = Actions::child_signal;
struct ExitObserver final : KernelExtension
{
    usize calls{};
    sched::ProcessId last{};
    bool fail{};
    Status process_exited(Kernel &, sched::ProcessId pid) override
    {
        ++calls; last = pid;
        return fail ? Status::io_error("injected extension failure") : Status::success();
    }
};
void frame(sched::ThreadControlBlock &thread)
{
    thread.native_frame = {}; thread.native_frame.flags = 0x202;
    thread.native_frame.sp = hw::user_begin + 8192; thread.native_frame.pc = site;
    const auto fp = native::X86SignalFrame::reset_fp();
    for (usize i = 0; i < fp.size(); ++i) thread.native_frame.fp_state[i] = static_cast<u8>(fp[i]);
}
u64 create(Kernel &kernel)
{
    const auto made = kernel.scheduler().create_user_process("admin-review",
        kernel.arch().make_user_context({site, hw::user_begin + 8192})); CHECK(made);
    auto &process = *kernel.scheduler().find(made.value());
    process.resources().personality = sched::UserPersonality::linux;
    process.resources().native_space = {0x100000 + process.pid() * 4096, static_cast<u32>(process.pid() % 16), 1};
    CHECK(native::allocate(process.resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    auto &thread = process.threads()[0]; CHECK(native::prepare_native_thread(thread)); thread.native_active = true; frame(thread);
    CHECK(native::private_signal_process(kernel, process.pid()));
    CHECK(kernel.scheduler().set_runnable(process.pid()));
    native::runtime_kernel = &kernel; native::started = true; return process.pid();
}
void activate(Kernel &kernel, u64 pid, u64 tid)
{ CHECK(kernel.posix().activate(pid)); CHECK(kernel.scheduler().activate_thread(pid, tid)); }
void idle(Kernel &kernel)
{ CHECK(kernel.posix().activate(0)); }
u64 fork(Kernel &kernel, u64 parent, u64 caller = 0)
{
    activate(kernel, parent, caller ? caller : parent); mock::spaces_enabled = true;
    bool complete = true; const auto result = native::dispatch(kernel, parent, {OK_SYS_FORK, {}}, complete);
    CHECK(result > 0 && complete); idle(kernel); return static_cast<u64>(result);
}
u64 peer(Kernel &kernel, u64 pid)
{
    const auto result = kernel.scheduler().create_thread_prepared(pid, kernel.scheduler().find(pid)->threads()[0].context,
        [](auto &thread) { thread.native_active = true; frame(thread); return native::prepare_native_thread(thread); });
    CHECK(result); return result.value();
}
void action(Kernel &kernel, u64 pid, u64 disposition, u64 flags = 0)
{
    auto *entry = native::private_signal_table(kernel, pid)->find(sigchld); CHECK(entry);
    *entry = {}; entry->handler = disposition; entry->flags = flags;
    if (disposition > 1) { entry->flags |= 0x04000004; entry->restorer = handler + 4; }
}
native::PrivateProcessQueue::Delivery pending(u64 pid)
{
    native::PrivateProcessQueue::Delivery ticket;
    CHECK(native::private_signal_find(pid)->shared.peek({}, ticket)); return ticket;
}
void raw_exit(Kernel &kernel, u64 pid, u64 tid, u64 code)
{
    activate(kernel, pid, tid); bool complete = true;
    CHECK(native::dispatch(kernel, pid, {OK_SYS_EXIT, {code}}, complete) == 0 && !complete);
    CHECK(native::runtime_status()); idle(kernel);
}
void release(Kernel &kernel, u64 pid)
{
    // Fixture cleanup is not the administrative behavior being asserted.
    native::release_process(kernel, pid);
    const auto *process = kernel.scheduler().find(pid);
    if (process && process->state() != sched::ProcessState::zombie) CHECK(kernel.scheduler().exit_process(pid, 0));
    CHECK(kernel.scheduler().reap_process(pid));
    CHECK(native::runtime_status());
}
void matrix(Kernel &kernel, ExitObserver &observer)
{
    for (u64 disposition : {u64{0}, u64{1}, handler})
        for (bool blocked : {false, true}) for (bool no_wait : {false, true})
        {
            const auto parent = create(kernel); action(kernel, parent, disposition, no_wait ? Actions::no_child_wait : 0);
            kernel.scheduler().find_thread(parent, parent)->native_signal_mask.set(sigchld, blocked);
            const auto child = fork(kernel, parent);
            auto *original_parent = kernel.scheduler().find(parent), *original_child = kernel.scheduler().find(child);
            const auto before = native::entry_stacks.accounting(); const auto calls = observer.calls;
            expect(kernel.kill_process(child).ok());
            expect(native::runtime_status().ok() && native::started);
            expect(observer.calls == calls + 1 && observer.last == child);
            expect(kernel.scheduler().find(parent) == original_parent && kernel.scheduler().find(child) == original_child);
            expect(original_child->state() == sched::ProcessState::zombie && original_child->wait_status() == 9);
            expect(!original_child->resources().native_space.root && original_child->resources().native_pages.empty());
            expect(!original_child->threads()[0].native_active && !original_child->threads()[0].entry_stack.pool);
            expect(native::entry_stacks.accounting().leased_slots + 1 == before.leased_slots);
            expect(native::entry_stacks.accounting().releases == before.releases + 1);
            const bool notified = disposition != 1 && (disposition > 1 || blocked);
            const bool autoreap = disposition == 1 || no_wait;
            expect(native::private_signal_find(parent)->shared.pending_mask().contains(sigchld) == notified);
            if (notified)
            {
                const auto info = pending(parent).info;
                expect(info.sender_pid == child && info.code == 2 && info.child_status == 9);
            }
            expect(original_child->parent_pid() == (autoreap ? 0 : parent));
            // A second administrative request must preserve the existing child
            // completion and the service, including when the first detached it.
            expect(!kernel.kill_process(child));
            expect(native::runtime_status().ok() && native::started && observer.calls == calls + 1);
            expect(kernel.scheduler().find(child) == original_child);
            expect(native::private_child_reap(kernel));
            if (!autoreap)
            {
                expect(kernel.scheduler().find(child) == original_child);
                activate(kernel, parent, parent); bool complete = true;
                expect(native::dispatch(kernel, parent, {OK_SYS_WAIT4, {child, output, 0, 0}}, complete) == static_cast<i64>(child));
                expect(complete); u32 status{}; std::memcpy(&status, mock::memory.data() + output - hw::user_begin, sizeof(status));
                expect(status == 9); idle(kernel);
            }
            expect(!kernel.scheduler().find(child)); release(kernel, parent);
        }
}
void leader_cause(Kernel &kernel)
{
    const auto parent = create(kernel); action(kernel, parent, handler);
    const auto child = fork(kernel, parent), last = peer(kernel, child);
    raw_exit(kernel, child, child, 41);
    expect(kernel.scheduler().find_thread(child, last)->native_active);
    expect(kernel.kill_process(child).ok());
    const auto info = pending(parent).info;
    expect(info.sender_pid == child && info.code == 1 && info.child_status == 41);
    expect(kernel.scheduler().find(child)->wait_status() == 9);
    CHECK(kernel.scheduler().wait_process(parent, child)); release(kernel, parent);
}
void blocked_child(Kernel &kernel)
{
    const auto parent = create(kernel); action(kernel, parent, handler);
    activate(kernel, parent, parent); const auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    const auto child = fork(kernel, parent);
    activate(kernel, child, child); auto &thread = *kernel.scheduler().find_thread(child, child);
    thread.syscall_pending = true; bool complete = true;
    expect(native::dispatch(kernel, child, {OK_SYS_READ, {static_cast<u64>(pipes.value()[0]), output, 1}}, complete) == -11);
    expect(!complete && thread.wait_reason == sched::ThreadWaitReason::pipe_read);
    const auto object = native::pipe_read_object(child, child); CHECK(object.pool);
    const auto saved = *native::pipe_read_objects.resolve(object).value();
    expect(kernel.scheduler().open_description(saved.description)->references == 3);
    idle(kernel); expect(kernel.kill_process(child).ok());
    expect(!native::pipe_read_object(child, child).pool && !native::pipe_read_objects.resolve(object));
    expect(native::pipe_read_objects.accounting().live_objects == 0);
    expect(kernel.scheduler().open_description(saved.description)->references == 1);
    expect(kernel.scheduler().find_thread(child, child)->state == sched::ProcessState::exited);
    expect(pending(parent).info.sender_pid == child);
    CHECK(kernel.scheduler().wait_process(parent, child)); release(kernel, parent);
}
void trusted_admin_boundary(Kernel &kernel)
{
    const auto sender = create(kernel), target = create(kernel);
    kernel.scheduler().find(sender)->set_credentials({.uid=7, .euid=8, .suid=9});
    kernel.scheduler().find(target)->set_credentials({.uid=11, .euid=12, .suid=13});
    const auto &caller = *kernel.scheduler().find(sender);
    expect(native::private_process_kill(kernel, caller, target, 9) == -1);
    expect(!native::private_signal_find(target)->group_kill);
    expect(kernel.scheduler().find(target)->state() != sched::ProcessState::zombie);
    expect(native::runtime_status().ok());
    idle(kernel); expect(kernel.kill_process(target).ok());
    expect(kernel.scheduler().find(target)->wait_status() == 9);
    expect(native::private_child_reap(kernel));
    expect(!kernel.scheduler().find(target)); release(kernel, sender);
}
void callback_failure(Kernel &kernel, ExitObserver &observer)
{
    const auto parent = create(kernel); action(kernel, parent, handler);
    const auto child = fork(kernel, parent); const auto calls = observer.calls;
    observer.fail = true; expect(!kernel.kill_process(child)); observer.fail = false;
    expect(native::runtime_status().ok() && native::started);
    expect(observer.calls == calls + 1 && kernel.scheduler().find(child)->state() == sched::ProcessState::zombie);
    const auto ticket = pending(parent);
    expect(!kernel.kill_process(child));
    expect(observer.calls == calls + 1 && native::runtime_status().ok());
    expect(pending(parent).info == ticket.info);
    CHECK(kernel.scheduler().wait_process(parent, child)); release(kernel, parent);
}
void rejected_requests(Kernel &kernel, ExitObserver &observer)
{
    const auto pid = create(kernel); const auto calls = observer.calls;
    const auto before = native::entry_stacks.accounting();
    expect(!kernel.kill_process(0)); expect(!kernel.kill_process(0xffffffffffULL));
    expect(native::runtime_status().ok() && native::started && observer.calls == calls);
    expect(native::entry_stacks.accounting().leased_slots == before.leased_slots);
    release(kernel, pid);
}
void runtime_guards(Kernel &kernel, ExitObserver &observer)
{
    const auto parent = create(kernel), child = fork(kernel, parent);
    const auto before = native::entry_stacks.accounting(); const auto calls = observer.calls;
    const auto untouched = [&] {
        expect(native::runtime_status().ok());
        expect(observer.calls == calls);
        expect(kernel.scheduler().find(child)->state() != sched::ProcessState::zombie);
        expect(kernel.scheduler().find(child)->resources().native_space.root != 0);
        expect(native::entry_stacks.accounting().leased_slots == before.leased_slots);
    };
    expect(!native::terminate_native_process(kernel, child, 0)); untouched();
    expect(!native::terminate_native_process(kernel, child, 65)); untouched();
    native::runtime_kernel = nullptr;
    expect(!kernel.kill_process(child)); untouched(); native::runtime_kernel = &kernel;
    native::started = false;
    expect(!kernel.kill_process(child)); untouched(); native::started = true;
    const auto lease = kernel.scheduler().find_thread(parent, parent)->entry_stack;
    const auto active = native::entry_stacks.activate_entry(lease, {parent, parent}, 0); CHECK(active);
    expect(!kernel.kill_process(child)); untouched();
    expect(native::entry_stacks.accounting().active_slots == 1);
    CHECK(native::entry_stacks.deactivate_entry(lease, {parent, parent}, 0));
    auto &child_thread = *kernel.scheduler().find_thread(child, child);
    const auto child_lease = child_thread.entry_stack; const auto child_top = child_thread.kernel_stack_top;
    child_thread.entry_stack = {}; child_thread.kernel_stack_top = 0;
    expect(!kernel.kill_process(child)); untouched();
    child_thread.entry_stack = child_lease; child_thread.kernel_stack_top = child_top;
    child_thread.native_active = false;
    expect(!kernel.kill_process(child)); untouched(); child_thread.native_active = true;
    native::runtime_failure = Status::io_error("preexisting service failure");
    expect(!kernel.kill_process(child));
    expect(native::runtime_status().code() == StatusCode::io_error);
    expect(observer.calls == calls && kernel.scheduler().find(child)->resources().native_space.root != 0);
    native::runtime_failure = Status::success();
    release(kernel, child); release(kernel, parent);
}
void unrelated_cleanup(Kernel &kernel, ExitObserver &observer)
{
    const auto made = kernel.scheduler().create_user_process("model-only-infrastructure",
        kernel.arch().make_user_context({site, hw::user_begin + 8192})); CHECK(made);
    CHECK(kernel.scheduler().set_runnable(made.value()));
    const auto calls = observer.calls; const auto before = native::entry_stacks.accounting();
    native::runtime_failure = Status::io_error("unrelated native service failure"); native::started = false;
    expect(kernel.kill_process(made.value()).ok());
    expect(!kernel.scheduler().find(made.value()));
    expect(native::runtime_status().code() == StatusCode::io_error && !native::started);
    expect(observer.calls == calls + 1);
    expect(native::entry_stacks.accounting().leased_slots == before.leased_slots);
    native::runtime_failure = Status::success();
}
void cleanup_failure(Kernel &kernel, ExitObserver &observer)
{
    const auto parent = create(kernel); action(kernel, parent, handler, Actions::no_child_wait);
    const auto child = fork(kernel, parent); const auto calls = observer.calls;
    mock::fail_destroy_root = kernel.scheduler().find(child)->resources().native_space.root;
    expect(!kernel.kill_process(child));
    expect(!native::runtime_status() && !native::started && observer.calls == calls);
    expect(kernel.scheduler().find(child)->state() != sched::ProcessState::zombie);
    expect(kernel.scheduler().find(child)->parent_pid() == parent);
    expect(!native::private_signal_find(parent)->shared.pending_mask().contains(sigchld));
    const auto entries = mock::entries; expect(!native::step(kernel) && mock::entries == entries);
    mock::fail_destroy_root = 0; native::runtime_failure = Status::success();
    release(kernel, child); release(kernel, parent);
}
}
int main()
{
    static Kernel kernel; static ExitObserver observer;
    KernelConfig config; config.extension = &observer; CHECK(kernel.boot(config));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    matrix(kernel, observer); leader_cause(kernel); blocked_child(kernel); trusted_admin_boundary(kernel); callback_failure(kernel, observer);
    rejected_requests(kernel, observer); runtime_guards(kernel, observer); unrelated_cleanup(kernel, observer); cleanup_failure(kernel, observer);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    std::printf("ADMIN_REVIEW_PASS checks=%zu hosted=1 native=0 test_points=%d private_fixture=0\n", checks,
#ifdef OK_ENABLE_TEST_POINTS
        1
#else
        0
#endif
    );
}
