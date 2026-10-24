// Actual ordinary runtime with hosted trap/mapping backends; no guest execution.
#define OK_NATIVE_SIGNAL_RUNTIME 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_child_exit_runtime_fixture.hpp"

namespace
{
using N = syscall::Number;
u64 negative(u64 group) { return static_cast<u64>(-static_cast<i64>(group)); }
void identity(Kernel &kernel, u64 pid, u32 uid)
{
    auto credentials = kernel.scheduler().find(pid)->credentials();
    credentials.uid = credentials.euid = credentials.suid = uid;
    CHECK(kernel.scheduler().set_credentials(pid, credentials));
}
void personality(Kernel &kernel, u64 pid, sched::UserPersonality value)
{ kernel.scheduler().find(pid)->resources().personality = value; }
i64 native_kill(Kernel &kernel, u64 sender, u64 selector, u64 signal)
{
    activate(kernel, sender, sender);
    bool complete = true;
    const auto result = native::dispatch(kernel, sender,
        {static_cast<uptr>(N::kill), {selector, signal}}, complete);
    expect(complete);
    return result;
}
i64 linux_kill(Kernel &kernel, u64 sender, u64 selector, u64 signal)
{
    activate(kernel, sender, sender);
    auto &thread = *kernel.scheduler().find_thread(sender, sender);
    thread.native_frame.registers[14] = *lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::kill);
    thread.native_frame.registers[9] = selector;
    thread.native_frame.registers[10] = signal;
    bool complete = true;
    i64 result = -999;
    expect(native::private_signal_call(kernel, *kernel.scheduler().find(sender), thread, complete, result));
    expect(complete);
    return result;
}
usize signal_owners()
{
    usize count = 0;
    for (const auto &state : native::private_signal_actions) count += state.pid != 0;
    return count;
}
void grouped(Kernel &kernel, u64 parent, u64 child, u64 group)
{ CHECK(kernel.scheduler().set_process_group(parent, static_cast<i32>(child), static_cast<i32>(group))); }

void selector_model()
{
    static sched::Scheduler scheduler;
    const auto context = arch::configured_arch_operations().make_user_context({site, hw::user_begin + 8192});
    std::array<u64, sched::max_processes> pids{};
    auto first = scheduler.create_user_process("group-snapshot-model", context); CHECK(first);
    pids[0] = first.value(); CHECK(scheduler.set_runnable(first.value()));
    for (usize index = 1; index < pids.size(); ++index)
    {
        const auto child = scheduler.fork_process(pids[index - 1]); CHECK(child);
        pids[index] = child.value();
    }
    const auto sender = user::process_signal_sender(*scheduler.find(pids[0]));
    usize count = 0;
    const auto no_delivery = [&](user::ProcessSignalSender origin, u64 target, i32 signal) -> i64 {
        expect(origin.pid == pids[0] && origin.group == pids[0] && signal == 0);
        expect(target == pids[count++]);
        return 0;
    };
    expect(user::send_process_group_signal<64>(scheduler, sender, 0, 0, no_delivery) == 0);
    expect(count == sched::max_processes);
    count = 0;
    expect(user::send_process_group_signal<64>(scheduler, sender,
        (u64{0x12345678} << 32) | static_cast<u32>(negative(pids[0])), u64{1} << 32, no_delivery) == 0);
    expect(count == sched::max_processes);
    count = 0;
    expect(user::send_process_group_signal<64>(scheduler, sender, 0, 15,
        [&](user::ProcessSignalSender, u64, i32) -> i64 { return ++count == 1 ? -1 : -12; }) == -12);
    expect(count == sched::max_processes);
    count = 0;
    expect(user::send_process_group_signal<64>(scheduler, sender, 0, 15,
        [&](user::ProcessSignalSender, u64, i32) -> i64 { return ++count == 2 ? 0 : -1; }) == 0);
    expect(count == sched::max_processes);
    // The callback really compacts canonical storage, including the sender.
    count = 0;
    expect(user::send_process_group_signal<64>(scheduler, sender, 0, 0,
        [&](user::ProcessSignalSender origin, u64 target, i32) -> i64 {
            expect(origin.pid == pids[0] && target == pids[count++]);
            CHECK(scheduler.find(target));
            CHECK(scheduler.exit_process(target, 0));
            CHECK(scheduler.reap_process(target));
            return 0;
        }) == 0);
    expect(count == sched::max_processes && scheduler.processes().empty());
}

template <lu::Profile P>
void profile_selector(Kernel &kernel, u64 sender)
{
    const auto origin = user::process_signal_sender(*kernel.scheduler().find(sender));
    usize calls = 0;
    auto callback = [&](user::ProcessSignalSender, u64, i32) -> i64 { ++calls; return 0; };
    using Codec = user::native::SignalActionCodec<P>;
    expect(user::send_process_group_signal<Codec::max_signal>(kernel.scheduler(), origin,
        0xfeedface80000000ULL, 0, callback) == -3);
    expect(user::send_process_group_signal<Codec::max_signal>(kernel.scheduler(), origin,
        0xfeedfaceffffffffULL, 0, callback) == -95);
    expect(user::send_process_group_signal<Codec::max_signal>(kernel.scheduler(), origin,
        0, 0xfeedfaceffffffffULL, callback) == -22);
    expect(user::send_process_group_signal<Codec::max_signal>(kernel.scheduler(), origin,
        0, Codec::max_signal + 1, callback) == -22);
    expect(user::send_process_group_signal<Codec::max_signal>(kernel.scheduler(), origin,
        0, 2, callback) == -95);
    expect(calls == 0);
}

void mixed_permission_and_zero(Kernel &kernel)
{
    const auto sender = create(kernel);
    const auto allowed = fork(kernel, sender), denied = fork(kernel, sender);
    grouped(kernel, sender, allowed, allowed);
    grouped(kernel, sender, denied, allowed);
    personality(kernel, allowed, sched::UserPersonality::native);
    identity(kernel, sender, 1000); identity(kernel, allowed, 1000); identity(kernel, denied, 0);
    native::private_signal_retire(allowed); native::private_signal_retire(denied);
    const auto owners = signal_owners();
    expect(native_kill(kernel, sender, negative(allowed), 0) == 0);
    expect(linux_kill(kernel, sender, negative(allowed), u64{0x12345678} << 32) == 0);
    expect(signal_owners() == owners && !native::private_signal_find(allowed) && !native::private_signal_find(denied));
    expect(native_kill(kernel, sender, negative(allowed), 2) == -95);
    expect(linux_kill(kernel, sender, negative(allowed), 65) == -22);
    expect(native_kill(kernel, sender, 0xffffffffU, 0) == -95);
    expect(linux_kill(kernel, sender, 0x80000000U, 0) == -3);
    expect(native_kill(kernel, sender, negative(sched::max_task_id), 0) == -3);
    // Linux group dispatch reaches an allowed native recipient; denied Linux peer stays live.
    expect(linux_kill(kernel, sender, negative(allowed), 15) == 0);
    expect(native::private_signal_find(allowed)->native_termination == 15);
    expect(!native::private_signal_find(denied));
    expect(kernel.scheduler().find(allowed)->state() != sched::ProcessState::zombie);
    native::private_process_signal_fatal(kernel);
    expect(kernel.scheduler().find(allowed)->state() == sched::ProcessState::zombie);
    expect(kernel.scheduler().find(allowed)->wait_status() == 15);
    expect(kernel.scheduler().find(denied)->state() != sched::ProcessState::zombie);
    identity(kernel, sender, 2000);
    expect(native_kill(kernel, sender, negative(allowed), 0) == -1);
    expect(linux_kill(kernel, sender, negative(allowed), 9) == -1);
    expect(!native::private_signal_find(denied));
    release(kernel, denied); release(kernel, allowed); release(kernel, sender);
}

void queued_linux_and_positive_regressions(Kernel &kernel)
{
    const auto sender = create(kernel), child = fork(kernel, sender);
    grouped(kernel, sender, child, child);
    auto *term = native::private_signal_table(kernel, child)->find(15); CHECK(term);
    *term = {}; term->handler = handler; term->flags = 0x04000004; term->restorer = handler + 4;
    kernel.scheduler().find_thread(child, child)->native_signal_mask.set(15);
    expect(native_kill(kernel, sender, negative(child), 15) == 0);
    auto *state = native::private_signal_find(child); CHECK(state);
    expect(state->shared.pending_mask().contains(15) && !state->native_termination && !state->group_kill);
    auto *wider = state->table.find(10); CHECK(wider); *wider = {}; wider->handler = 1;
    expect(linux_kill(kernel, sender, child, 10) == 0);
    expect(native_kill(kernel, sender, child, 10) == -95);
    expect(linux_kill(kernel, sender, negative(child), 10) == -95);
    // Signal zero cannot overwrite or consume accepted pending TERM.
    const auto owners = signal_owners();
    expect(linux_kill(kernel, sender, negative(child), 0) == 0);
    expect(signal_owners() == owners && state->shared.pending_mask().contains(15));
    expect(native_kill(kernel, sender, negative(child), 9) == 0);
    expect(state->group_kill);
    native::private_process_signal_fatal(kernel);
    expect(kernel.scheduler().find(child)->state() == sched::ProcessState::zombie);
    expect(kernel.scheduler().find(child)->wait_status() == 9);
    release(kernel, child); release(kernel, sender);
}

void self_and_blocked(Kernel &kernel)
{
    const auto sender = create(kernel), child = fork(kernel, sender);
    personality(kernel, sender, sched::UserPersonality::native);
    activate(kernel, child, child);
    auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::console_read, 0); CHECK(ticket);
    kernel.scheduler().find_thread(child, child)->syscall_pending = true;
    expect(native_kill(kernel, sender, 0, 9) == 0);
    expect(kernel.scheduler().find(sender)->state() != sched::ProcessState::zombie);
    expect(kernel.scheduler().find_thread(sender, sender)->native_active);
    expect(kernel.scheduler().find(child)->state() == sched::ProcessState::blocked);
    expect(native::private_signal_find(sender)->native_termination == 9 && native::private_signal_find(child)->group_kill);
    native::private_process_signal_fatal(kernel);
    for (auto pid : {sender, child})
    {
        const auto *process = kernel.scheduler().find(pid); CHECK(process);
        expect(process->state() == sched::ProcessState::zombie && process->wait_status() == 9);
        for (const auto &thread : process->threads()) expect(!thread.native_active && !thread.entry_stack.pool);
    }
    expect(kernel.scheduler().find(sched::user_init_pid)->state() != sched::ProcessState::zombie);
    release(kernel, child); release(kernel, sender);
}

void reaped_leader_and_zombie(Kernel &kernel)
{
    const auto sender = create(kernel), leader = fork(kernel, sender);
    grouped(kernel, sender, leader, leader);
    const auto member = fork(kernel, leader);
    personality(kernel, member, sched::UserPersonality::native);
    expect(native_kill(kernel, sender, leader, 9) == 0);
    native::private_process_signal_fatal(kernel);
    CHECK(kernel.scheduler().wait_process(sender, leader));
    expect(!kernel.scheduler().find(leader) && kernel.scheduler().find(member)->process_group_id() == leader);
    native::private_signal_retire(member);
    const auto owners = signal_owners();
    expect(linux_kill(kernel, sender, negative(leader), 0) == 0 && signal_owners() == owners);
    expect(native_kill(kernel, sender, negative(leader), 15) == 0);
    native::private_process_signal_fatal(kernel);
    expect(kernel.scheduler().find(member)->state() == sched::ProcessState::zombie);
    const auto zombie_owners = signal_owners();
    expect(linux_kill(kernel, sender, negative(leader), 0) == 0 && signal_owners() == zombie_owners);
    expect(native_kill(kernel, sender, negative(leader), 9) == 0 && signal_owners() == zombie_owners);
    release(kernel, member);
    expect(linux_kill(kernel, sender, negative(leader), 0) == -3);
    release(kernel, sender);
}
} // namespace

int main()
{
    static_assert(!native::native_signal_diagnostics);
    selector_model();
    static Kernel kernel; CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    const auto init = create_init(kernel);
    profile_selector<lu::Profile::i386>(kernel, init);
    profile_selector<lu::Profile::x86_64>(kernel, init);
    profile_selector<lu::Profile::aarch64>(kernel, init);
    profile_selector<lu::Profile::arm32>(kernel, init);
    profile_selector<lu::Profile::rv64>(kernel, init);
    profile_selector<lu::Profile::rv32>(kernel, init);
    profile_selector<lu::Profile::loongarch64>(kernel, init);
    profile_selector<lu::Profile::mips>(kernel, init);
    profile_selector<lu::Profile::mips64>(kernel, init);
    profile_selector<lu::Profile::ppc>(kernel, init);
    mixed_permission_and_zero(kernel);
    queued_linux_and_positive_regressions(kernel);
    self_and_blocked(kernel);
    reaped_leader_and_zombie(kernel);
    expect(native::runtime_status().ok());
    // Original init is deliberately retained for recovery and cannot be reaped.
    expect(kernel.scheduler().find(init)->state() != sched::ProcessState::zombie);
    std::printf("NATIVE_GROUP_SIGNAL_RUNTIME_PASS checks=%zu profiles=10 snapshot_capacity=64 native_linux=1 guest=0\n", checks);
}
