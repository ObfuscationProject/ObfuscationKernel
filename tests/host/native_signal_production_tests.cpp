// Actual ordinary runtime; the build selects Debug/Release diagnostic gates.
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_child_exit_runtime_fixture.hpp"

#ifndef OK_NATIVE_SIGNAL_RUNTIME
#error "Ordinary signal runtime tests require the production runtime definition"
#endif
#ifdef OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE
#error "Ordinary signal runtime tests cannot select private diagnostics"
#endif
static_assert(!native::native_signal_diagnostics);
namespace capture {
std::array<char, 65536> serial{};
usize used{};
}
extern "C" void ok_platform_console_write_char(char value)
{
    CHECK(capture::used < capture::serial.size());
    capture::serial[capture::used++] = value;
}


namespace {
void admin_policy(Kernel &kernel)
{
    for (u64 disposition : {u64{0}, u64{1}, handler})
        for (bool blocked : {false, true}) for (bool no_wait : {false, true})
        {
            const auto parent = create(kernel); action(kernel, parent, disposition, no_wait ? Actions::no_child_wait : 0);
            kernel.scheduler().find_thread(parent, parent)->native_signal_mask.set(sigchld, blocked);
            const auto child = fork(kernel, parent);
            expect(kernel.kill_process(child).ok());
            const auto *completed = kernel.scheduler().find(child); expect(completed != nullptr);
            expect(completed->state() == sched::ProcessState::zombie && completed->wait_status() == 9);
            expect(!completed->resources().native_space.root && completed->resources().native_pages.empty());
            expect(!completed->threads()[0].entry_stack.pool && !native::private_signal_find(child));
            const bool notified = disposition != 1 && (disposition > 1 || blocked);
            const bool autoreap = disposition == 1 || no_wait;
            expect(native::private_signal_find(parent)->shared.pending_mask().contains(sigchld) == notified);
            if (notified)
            {
                const auto info = pending(parent).info;
                expect(info.sender_pid == child && info.code == 2 && info.child_status == 9);
            }
            expect(completed->parent_pid() == (autoreap ? 0 : parent));
            expect(kernel.kill_process(child).code() == StatusCode::already_exists);
            expect(native::runtime_status().ok());
            if (autoreap) expect(native::private_child_reap(kernel));
            else { const auto waited = kernel.scheduler().wait_process(parent, child); expect(waited.ok()); }
            expect(!kernel.scheduler().find(child)); release(kernel, parent);
        }
}
void admin_lineage(Kernel &kernel)
{
    const auto parent = create(kernel); action(kernel, parent, handler);
    const auto child = fork(kernel, parent); peer(kernel, child);
    raw_exit(kernel, child, child, 17); expect(kernel.kill_process(child).ok());
    expect(pending(parent).info.child_status == 17 && pending(parent).info.code == 1);
    expect(kernel.scheduler().find(child)->wait_status() == 9);
    CHECK(kernel.scheduler().wait_process(parent, child)); release(kernel, parent);
    const auto orphan_parent = create(kernel), orphan = fork(kernel, orphan_parent);
    expect(kernel.kill_process(orphan_parent).ok()); expect(native::private_child_reap(kernel));
    expect(!kernel.scheduler().find(orphan_parent) && kernel.scheduler().find(orphan)->parent_pid() == 0);
    expect(kernel.kill_process(orphan).ok()); expect(native::private_child_reap(kernel));
    expect(!kernel.scheduler().find(orphan));
}
void admin_failure(Kernel &kernel)
{
    const auto parent = create(kernel); action(kernel, parent, handler, Actions::no_child_wait);
    const auto child = fork(kernel, parent);
    expect(kernel.kill_process(0).code() == StatusCode::invalid_argument);
    expect(kernel.kill_process(0x7fffffff).code() == StatusCode::not_found);
    native::runtime_kernel = nullptr;
    expect(kernel.kill_process(child).code() == StatusCode::not_initialized);
    native::runtime_kernel = &kernel;
    auto &thread = *kernel.scheduler().find_thread(child, child);
    const auto lease = thread.entry_stack;
    auto active = native::entry_stacks.activate_entry(lease, {child, child}, 0); expect(active.ok());
    expect(kernel.kill_process(child).code() == StatusCode::busy);
    CHECK(native::entry_stacks.deactivate_entry(lease, {child, child}, 0));
    expect(native::runtime_status().ok());
    mock::fail_destroy_root = kernel.scheduler().find(child)->resources().native_space.root;
    expect(!kernel.kill_process(child)); expect(!native::runtime_status() && !native::started);
    expect(kernel.scheduler().find(child)->state() != sched::ProcessState::zombie);
    expect(kernel.scheduler().find(child)->parent_pid() == parent);
    expect(!native::private_signal_find(parent)->shared.pending_mask().contains(sigchld));
    const auto entries = mock::entries; expect(!native::step(kernel) && mock::entries == entries);
    native::runtime_failure = Status::success(); release(kernel, child); release(kernel, parent);
}
void ordinary_signal(Kernel &kernel)
{
    const auto pid = create(kernel); auto &process = *kernel.scheduler().find(pid); auto &thread = process.threads()[0];
    auto *entry = native::private_signal_table(kernel, pid)->find(10); entry->handler = handler;
    entry->flags = 0x04000004; entry->restorer = handler + 4;
    expect(native::private_process_kill(kernel, process, pid, 10) == 0);
    expect(native::private_signal_deliver(kernel, process, thread)); expect(thread.native_frame.pc == handler);
    expect(thread.native_signal_mask.contains(10));
    expect(native::private_process_kill(kernel, process, 0, 10) == -95);
    expect(native::private_process_kill(kernel, process, pid, Actions::stop_signal) == -95);
    expect(native::private_process_kill(kernel, process, pid, Actions::continue_signal) == -95);
    release(kernel, pid);
}
u64 native_process(Kernel &kernel)
{
    const auto pid = create(kernel);
    kernel.scheduler().find(pid)->resources().personality = sched::UserPersonality::native;
    return pid;
}
i64 native_kill(Kernel &kernel, u64 sender, i32 target, i32 signal)
{
    activate(kernel, sender, sender);
    bool complete = true;
    const auto result = native::dispatch(kernel, sender,
        {OK_SYS_KILL, {static_cast<uptr>(target), static_cast<uptr>(signal)}}, complete);
    expect(complete);
    return result;
}
void native_kill_contract(Kernel &kernel)
{
    const auto sender = native_process(kernel), target = native_process(kernel);
    expect(native_kill(kernel, sender, 0, 0) == 0);
    expect(native_kill(kernel, sender, -1, OK_SIGKILL) == -95);
    expect(native_kill(kernel, sender, 0x7fffffff, 0) == -3);
    expect(native_kill(kernel, sender, target, -1) == -22);
    expect(native_kill(kernel, sender, target, OK_NATIVE_SIGNAL_MAX + 1) == -22);
    expect(native_kill(kernel, sender, target, 10) == -95);
    auto &source = *kernel.scheduler().find(sender);
    auto &recipient = *kernel.scheduler().find(target);
    source.set_credentials({.uid = 1001, .gid = 1001, .euid = 1002, .egid = 1001});
    recipient.set_credentials({.uid = 1003, .gid = 1003, .euid = 1004, .egid = 1003, .suid = 1005});
    expect(native_kill(kernel, sender, target, 0) == -1);
    expect(native_kill(kernel, sender, target, OK_SIGKILL) == -1);
    expect(native::private_signal_find(target)->native_termination == 0);
    auto credentials = recipient.credentials(); credentials.suid = 1002; recipient.set_credentials(credentials);
    expect(native_kill(kernel, sender, target, 0) == 0);
    expect(native::private_signal_find(target)->native_termination == 0);
    source.set_credentials(user::root_credentials());
    const auto task = kernel.scheduler().create_process("kill-control", kernel.arch().make_kernel_context(1, 2)); CHECK(task);
    expect(native_kill(kernel, sender, task.value(), 0) == -95);
    CHECK(kernel.scheduler().exit_process(task.value(), 0)); CHECK(kernel.scheduler().reap_process(task.value()));
    release(kernel, target); release(kernel, sender);
}
void native_kill_lifecycle(Kernel &kernel)
{
    for (const auto signal : {OK_SIGTERM, OK_SIGKILL})
    {
        const auto parent = native_process(kernel), child = fork(kernel, parent);
        const auto sibling = peer(kernel, child);
        activate(kernel, child, sibling);
        const auto pipe = kernel.posix().pipe(0); CHECK(pipe);
        auto &thread = *kernel.scheduler().find_thread(child, sibling); thread.syscall_pending = true;
        bool complete = true;
        expect(native::dispatch(kernel, child, {OK_SYS_READ,
            {static_cast<uptr>(pipe.value()[0]), info_address, 1}}, complete) == -11 && !complete);
        const auto ticket = thread.wait_registration.ticket();
        expect(native::pipe_read_object(child, sibling).pool != nullptr);
        expect(native_kill(kernel, parent, child, signal) == 0);
        expect(kernel.scheduler().find(child)->state() != sched::ProcessState::zombie);
        expect(native::private_signal_find(child)->native_termination == signal);
        native::private_process_signal_fatal(kernel);
        const auto *completed = kernel.scheduler().find(child); CHECK(completed);
        expect(completed->state() == sched::ProcessState::zombie && completed->wait_status() == signal);
        expect(completed->parent_pid() == parent && !completed->resources().native_space.root);
        expect(completed->resources().native_pages.empty() && !completed->threads()[0].entry_stack.pool);
        expect(!native::pipe_read_object(child, sibling).pool && !native::private_signal_find(child));
        expect(!kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
        expect(native::pipe_read_objects.accounting().live_objects == 0);
        expect(native_kill(kernel, parent, child, 0) == 0);
        expect(native_kill(kernel, parent, child, signal) == 0);
        const auto waited = kernel.scheduler().wait_process(parent, child); CHECK(waited);
        expect(!kernel.scheduler().find(child)); release(kernel, parent);
    }
    const auto self = native_process(kernel);
    expect(native_kill(kernel, self, self, OK_SIGTERM) == 0);
    expect(native_kill(kernel, self, self, OK_SIGKILL) == 0);
    expect(native_kill(kernel, self, self, OK_SIGTERM) == 0);
    auto *process = kernel.scheduler().find(self); CHECK(process);
    expect(process->state() == sched::ProcessState::running && process->threads()[0].native_active);
    expect(native::private_signal_find(self)->native_termination == OK_SIGKILL);
    native::private_process_signal_fatal(kernel);
    expect(kernel.scheduler().find(self)->wait_status() == OK_SIGKILL);
    release(kernel, self);
    // A native sender uses the Linux recipient's established pending-signal path.
    const auto sender = native_process(kernel), linux = create(kernel);
    expect(native_kill(kernel, sender, linux, OK_SIGKILL) == 0);
    expect(native::private_signal_find(linux)->group_kill);
    native::private_process_signal_fatal(kernel);
    expect(kernel.scheduler().find(linux)->wait_status() == OK_SIGKILL);
    release(kernel, linux); release(kernel, sender);
}
void native_admin_guards(Kernel &kernel)
{
    const auto target = native_process(kernel);
    auto &process = *kernel.scheduler().find(target);
    const auto before = native::entry_stacks.accounting().leased_slots;
    native::runtime_kernel = nullptr;
    expect(kernel.kill_process(target).code() == StatusCode::not_initialized);
    native::runtime_kernel = &kernel;
    expect(process.state() != sched::ProcessState::zombie && process.resources().native_space.root);
    expect(native::entry_stacks.accounting().leased_slots == before);
    process.set_execution(sched::ProcessExecution::kernel_thread);
    expect(native::terminate_native_process(kernel, target, OK_SIGKILL).code() == StatusCode::invalid_argument);
    process.set_execution(sched::ProcessExecution::user_process);
    expect(kernel.kill_process(target).ok() && !kernel.scheduler().find(target));
    expect(native::runtime_status().ok());

    const auto model = kernel.scheduler().create_user_process("native-admin-model-control",
        kernel.arch().make_user_context({site, hw::user_begin + 8192})); CHECK(model);
    CHECK(kernel.scheduler().set_runnable(model.value()));
    native::runtime_failure = Status::io_error("unrelated native service failure"); native::started = false;
    expect(kernel.kill_process(model.value()).ok() && !kernel.scheduler().find(model.value()));
    expect(native::runtime_status().code() == StatusCode::io_error && !native::started);
    native::runtime_failure = Status::success(); native::started = true;

    const auto unpublished = native_process(kernel);
    auto *rollback = kernel.scheduler().find(unpublished);
    rollback->set_state(sched::ProcessState::created);
    rollback->threads()[0].state = sched::ProcessState::created;
    rollback->threads()[0].native_active = false;
    expect(!native::terminate_native_process(kernel, unpublished, OK_SIGKILL));
    expect(kernel.kill_process(unpublished).ok() && !kernel.scheduler().find(unpublished));
    expect(native::runtime_status().ok());
}
void native_init_protection(Kernel &kernel)
{
    const auto sender = native_process(kernel), init = create_init(kernel);
    kernel.scheduler().find(init)->resources().personality = sched::UserPersonality::native;
    expect(native_kill(kernel, sender, init, 0) == 0);
    expect(native_kill(kernel, sender, init, OK_SIGTERM) == -1);
    expect(native_kill(kernel, sender, init, OK_SIGKILL) == -1);
    release(kernel, sender);
    // An unpublished fork rollback need not have acquired a signal owner.
    const auto unpublished = kernel.scheduler().create_user_process("unpublished-adoption-control",
        kernel.arch().make_user_context({site, hw::user_begin + 8192})); CHECK(unpublished);
    expect(kernel.scheduler().user_adoption_target(unpublished.value()) == 0);
    expect(!native::private_signal_find(unpublished.value()));
    native::finish(kernel, unpublished.value(), 127);
    expect(native::runtime_status().ok());
    expect(kernel.scheduler().find(unpublished.value())->state() == sched::ProcessState::zombie);
    CHECK(kernel.scheduler().reap_process(unpublished.value()));
    for (const auto personality : {sched::UserPersonality::native, sched::UserPersonality::linux})
    {
        const auto service = fork(kernel, init);
        kernel.scheduler().find(service)->resources().personality = personality;
        const auto descendant = fork(kernel, service), zombie = fork(kernel, service);
        raw_exit(kernel, zombie, zombie, 31, true);
        expect(kernel.scheduler().find(zombie)->parent_pid() == service);
        expect(kernel.scheduler().user_adoption_target(service) == init);
        raw_exit(kernel, service, service, 17, true);
        expect(kernel.scheduler().find(descendant)->parent_pid() == init);
        expect(kernel.scheduler().find(zombie)->parent_pid() == init);
        const auto *owner = native::private_signal_find(descendant); CHECK(owner);
        expect(owner->lineage.parent == init &&
               owner->lineage.parent_epoch == native::private_signal_find(init)->epoch &&
               owner->lineage.parent_tid == init);
        activate(kernel, descendant, descendant);
        bool complete = true;
        expect(native::dispatch(kernel, descendant, {OK_SYS_GETPPID, {}}, complete) == 1 && complete);
        raw_exit(kernel, descendant, descendant, 23, true);
        for (const auto child : {service, descendant, zombie})
        {
            activate(kernel, init, init); complete = true;
            expect(native::dispatch(kernel, init, {OK_SYS_WAIT4, {child, info_address, 0}}, complete)
                == static_cast<i64>(child) && complete);
            i32 status{}; std::memcpy(&status, mock::memory.data() + info_address - hw::user_begin, sizeof(status));
            expect(status == ((child == service ? 17 : child == descendant ? 23 : 31) << 8));
            expect(!kernel.scheduler().find(child));
        }
        expect(kernel.scheduler().find(init)->children().empty());
        expect(!native::private_signal_find(init)->shared.pending_mask().contains(sigchld));
        expect(native::runtime_status().ok());
    }
    // Native administrative cleanup must commit adoption before erasing its owner.
    for (const auto personality : {sched::UserPersonality::native, sched::UserPersonality::linux})
    {
        kernel.scheduler().find(init)->resources().personality = personality;
        action(kernel, init, handler);
        const auto service = fork(kernel, init);
        kernel.scheduler().find(service)->resources().personality = sched::UserPersonality::native;
        const auto descendant = fork(kernel, service), zombie = fork(kernel, service);
        kernel.scheduler().find(descendant)->resources().personality = sched::UserPersonality::linux;
        kernel.scheduler().find(zombie)->resources().personality = sched::UserPersonality::linux;
        raw_exit(kernel, zombie, zombie, 31, true);
        expect(kernel.kill_process(service).ok() && !kernel.scheduler().find(service));
        expect(kernel.scheduler().find(descendant)->parent_pid() == init &&
               kernel.scheduler().find(zombie)->parent_pid() == init);
        const auto *owner = native::private_signal_find(descendant); CHECK(owner);
        expect(owner->lineage.parent == init &&
               owner->lineage.parent_epoch == native::private_signal_find(init)->epoch &&
               owner->lineage.parent_tid == init);
        raw_exit(kernel, descendant, descendant, 23, true);
        expect(native::private_signal_find(init)->shared.pending_mask().contains(sigchld) ==
               (personality == sched::UserPersonality::linux));
        for (const auto child : {descendant, zombie})
        {
            activate(kernel, init, init); bool complete = true;
            expect(native::dispatch(kernel, init, {OK_SYS_WAIT4, {child, info_address, 0}}, complete)
                == static_cast<i64>(child) && complete);
            i32 status{}; std::memcpy(&status, mock::memory.data() + info_address - hw::user_begin, sizeof(status));
            expect(status == ((child == descendant ? 23 : 31) << 8));
            expect(!kernel.scheduler().find(child));
        }
        native::private_signal_find(init)->shared.discard(sigchld);
        expect(kernel.scheduler().find(init)->children().empty() && native::runtime_status().ok());
    }
    kernel.scheduler().find(init)->resources().personality = sched::UserPersonality::native;
    // PID 1 remains a terminal diagnostic record even in fixture cleanup.
    native::release_process(kernel, init);
    CHECK(kernel.scheduler().exit_process(init, 0));
    expect(kernel.scheduler().reap_process(init).code() == StatusCode::busy);
}
}
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    admin_policy(kernel); admin_lineage(kernel); admin_failure(kernel); ordinary_signal(kernel);
    native_kill_contract(kernel); native_kill_lifecycle(kernel); native_admin_guards(kernel);
    native_init_protection(kernel);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    const std::string_view serial{capture::serial.data(), capture::used};
    expect(!serial.empty());
    for (const auto marker : {"OK_NATIVE_CHILD_EXIT", "OK_NATIVE_CHILD_REAP", "OK_NATIVE_SIGNAL_WAIT",
                              "OK_NATIVE_SLEEP_INTERRUPT", "OK_NATIVE_CHILD_SIGNAL", "N64_PRIVATE_EARLY"})
        expect(serial.find(marker) == std::string_view::npos);
    std::printf("SIGNAL_PRODUCTION_PASS checks=%zu hosted=1 native=0 private_diagnostics=0\n", checks);
}
