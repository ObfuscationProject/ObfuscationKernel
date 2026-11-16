// Actual scheduler session and process-group mechanisms, and controlling
// terminals derived from sessions (hosted; no guest evidence).
#include "ok/sched/process.hpp"
#include "ok/sched/scheduler.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ok;
using namespace ok::sched;
namespace
{
usize checks{};
#define CHECK(condition) do { ++checks; if (!static_cast<bool>(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

struct Identity
{
    ProcessId pid{}, parent{}, group{}, session{};
    bool exec{};
    bool operator==(const Identity &) const = default;
};

struct Fixture
{
    std::unique_ptr<Scheduler> owned{std::make_unique<Scheduler>()};
    Scheduler &scheduler{*owned};
    arch::CpuContext context{arch::configured_arch_operations().make_user_context({0x400000, 0x800000})};
    Fixture()
    {
        auto init = scheduler.create_init_process(context);
        CHECK(init && init.value() == user_init_pid);
        CHECK(scheduler.set_runnable(init.value()));
    }
    ProcessId fork(ProcessId parent = user_init_pid)
    {
        auto child = scheduler.fork_process(parent);
        CHECK(child);
        return child.value();
    }
    ProcessControlBlock &process(ProcessId pid)
    {
        auto *process = scheduler.find(pid);
        CHECK(process);
        return *process;
    }
    auto identities() const
    {
        StaticVector<Identity, max_processes> result;
        for (const auto &process : scheduler.processes())
            CHECK(result.push_back({process.pid(), process.parent_pid(), process.process_group_id(),
                                    process.session_id(), process.has_execed()}));
        return result;
    }
    void ids(ProcessId pid, ProcessId group, ProcessId session)
    {
        const auto pgid = scheduler.process_group(user_init_pid, static_cast<i32>(pid));
        const auto sid = scheduler.session_id(user_init_pid, static_cast<i32>(pid));
        CHECK(pgid && pgid.value() == group);
        CHECK(sid && sid.value() == session);
    }
};

template <typename Operation>
void refuse(Fixture &fixture, Operation operation, StatusCode code)
{
    const auto before = fixture.identities();
    const auto result = operation();
    if constexpr (std::is_same_v<decltype(result), const Status>) CHECK(result.code() == code);
    else CHECK(!result && result.status().code() == code);
    const auto after = fixture.identities();
    CHECK(before.size() == after.size());
    for (usize index = 0; index < before.size(); ++index) CHECK(before[index] == after[index]);
}

void initial_membership_and_fork()
{
    ProcessControlBlock raw{19, "constructed"};
    CHECK(raw.process_group_id() == 19 && raw.session_id() == 19 && !raw.has_execed());
    CHECK(raw.configure(23, 19, "configured"));
    CHECK(raw.parent_pid() == 19 && raw.process_group_id() == 23 && raw.session_id() == 23 && !raw.has_execed());
    Fixture f;
    f.ids(1, 1, 1);
    const auto independent = f.scheduler.create_user_process("independent", f.context);
    CHECK(independent);
    f.ids(independent.value(), independent.value(), independent.value());
    const auto child = f.fork();
    f.ids(child, 1, 1);
    CHECK(f.scheduler.process_group(child, 0).value() == 1);
    CHECK(f.scheduler.session_id(child, 0).value() == 1);
    auto session = f.scheduler.create_session(child);
    CHECK(session && session.value() == child);
    f.ids(child, child, child);
    const auto grandchild = f.fork(child);
    f.ids(grandchild, child, child);
    CHECK(f.process(grandchild).parent_pid() == child);
    CHECK(f.scheduler.set_process_group(child, static_cast<i32>(grandchild), 0));
    f.ids(grandchild, grandchild, child);
    const auto descendant = f.fork(grandchild);
    f.ids(descendant, grandchild, child);
}

void validation_order_and_parent_authority()
{
    Fixture f;
    const auto child = f.fork(), sibling = f.fork(), foreign = f.fork();
    CHECK(f.scheduler.create_session(foreign));
    refuse(f, [&] { return f.scheduler.set_process_group(1, 0, 0); }, StatusCode::permission);
    refuse(f, [&] { return f.scheduler.create_session(1); }, StatusCode::permission);
    refuse(f, [&] { return f.scheduler.set_process_group(1, -1, 0); }, StatusCode::invalid_argument);
    refuse(f, [&] { return f.scheduler.set_process_group(1, -1, 1); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.set_process_group(1, 999999, -1); }, StatusCode::invalid_argument);
    refuse(f, [&] { return f.scheduler.set_process_group(1, 999999, 1); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.set_process_group(1, child, 999999); }, StatusCode::permission);
    refuse(f, [&] { return f.scheduler.set_process_group(child, sibling, -1); }, StatusCode::invalid_argument);
    refuse(f, [&] { return f.scheduler.set_process_group(child, sibling, 1); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.set_process_group(1, foreign, 1); }, StatusCode::permission);
    refuse(f, [&] { return f.scheduler.set_process_group(child, foreign, 1); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.process_group(child, -1); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.session_id(child, 999999); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.create_session(999999); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.set_process_group(999999, child, child); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.process_group(999999, child); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.session_id(999999, child); }, StatusCode::no_process);
    CHECK(f.scheduler.set_process_group(1, child, child));
    CHECK(f.scheduler.set_process_group(child, 0, 0));
    CHECK(f.scheduler.set_process_group(1, sibling, child));
    f.ids(child, child, 1);
    f.ids(sibling, child, 1);
    // Group membership is a relationship authority, independent of credentials.
    f.process(child).set_credentials(user::default_user_credentials());
    f.process(sibling).set_credentials(user::root_credentials());
    CHECK(f.scheduler.set_process_group(child, 0, 1));
    f.ids(child, 1, 1);
}

void successful_exec_marker_and_fork_reset()
{
    Fixture f;
    const auto child = f.fork();
    auto &process = f.process(child);
    CHECK(!process.has_execed());
    process.resources().prepared_descriptor_operations = 1;
    MemoryMap memory;
    refuse(f, [&] { return f.scheduler.replace_image(child, f.context, memory); }, StatusCode::busy);
    process.resources().prepared_descriptor_operations = 0;
    ProcessManager manager(f.scheduler);
    CHECK(!manager.execve(child, {}, f.context.architecture));
    CHECK(!process.has_execed());
    CHECK(f.scheduler.set_process_group(1, child, child));
    CHECK(f.scheduler.replace_image(child, f.context, memory));
    CHECK(process.has_execed());
    f.ids(child, child, 1);
    refuse(f, [&] { return f.scheduler.set_process_group(1, child, 1); }, StatusCode::denied);
    refuse(f, [&] { return f.scheduler.set_process_group(1, child, 999999); }, StatusCode::denied);
    // The child retains authority over itself after exec.
    CHECK(f.scheduler.set_process_group(child, 0, 1));
    const auto grandchild = f.fork(child);
    CHECK(!f.process(grandchild).has_execed() && process.has_execed());
    CHECK(f.scheduler.set_process_group(child, grandchild, grandchild));
    CHECK(f.scheduler.create_session(child));
    // Cross-session rejection precedes the child's successful-exec check.
    refuse(f, [&] { return f.scheduler.set_process_group(1, child, 1); }, StatusCode::permission);
    f.ids(grandchild, grandchild, 1);
}

void thread_and_retained_leader_identities()
{
    Fixture f;
    const auto child = f.fork();
    CHECK(f.scheduler.set_process_group(1, child, child));
    const auto peer = f.scheduler.create_thread(child, f.context);
    CHECK(peer);
    f.ids(peer.value(), child, 1);
    refuse(f, [&] { return f.scheduler.set_process_group(1, peer.value(), child); }, StatusCode::invalid_argument);
    refuse(f, [&] { return f.scheduler.set_process_group(1, peer.value(), -1); }, StatusCode::invalid_argument);
    CHECK(f.scheduler.exit_thread(child, peer.value()));
    refuse(f, [&] { return f.scheduler.process_group(1, peer.value()); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.session_id(1, peer.value()); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.set_process_group(1, peer.value(), child); }, StatusCode::no_process);
    const auto live = f.scheduler.create_thread(child, f.context);
    CHECK(live);
    CHECK(f.scheduler.exit_thread(child, child));
    f.ids(child, child, 1);
    f.ids(live.value(), child, 1);
    CHECK(f.scheduler.set_process_group(1, child, 1));
    f.ids(live.value(), 1, 1);
    CHECK(f.scheduler.exit_process(child, 0));
    f.ids(child, 1, 1);
    // Linux retains the zombie process identity and permits parent setpgid.
    CHECK(f.scheduler.set_process_group(1, child, child));
    f.ids(child, child, 1);
    CHECK(f.scheduler.reap_process(child));
    refuse(f, [&] { return f.scheduler.process_group(1, child); }, StatusCode::no_process);
    refuse(f, [&] { return f.scheduler.session_id(1, child); }, StatusCode::no_process);
}

void group_membership_reserves_identity_until_last_reap()
{
    Fixture f;
    const auto leader = f.fork();
    CHECK(f.scheduler.set_process_group(1, leader, leader));
    const auto member = f.fork(leader);
    CHECK(f.scheduler.set_process_group(leader, 0, 1));
    refuse(f, [&] { return f.scheduler.create_session(leader); }, StatusCode::permission);
    CHECK(f.scheduler.exit_process(member, 0));
    // A zombie continues to reserve its group even after its leader moved away.
    refuse(f, [&] { return f.scheduler.create_session(leader); }, StatusCode::permission);
    const auto joiner = f.fork();
    CHECK(f.scheduler.set_process_group(1, joiner, leader));
    CHECK(f.scheduler.reap_process(member));
    refuse(f, [&] { return f.scheduler.create_session(leader); }, StatusCode::permission);
    CHECK(f.scheduler.exit_process(joiner, 0));
    refuse(f, [&] { return f.scheduler.create_session(leader); }, StatusCode::permission);
    CHECK(f.scheduler.reap_process(joiner));
    CHECK(f.scheduler.create_session(leader));
    f.ids(leader, leader, leader);
}

void departed_leaders_reparent_and_compaction()
{
    Fixture f;
    const auto leader = f.fork();
    CHECK(f.scheduler.create_session(leader));
    const auto child = f.fork(leader);
    const auto member = f.fork(child);
    CHECK(f.scheduler.set_process_group(child, 0, 0));
    CHECK(f.scheduler.set_process_group(child, member, child));
    f.ids(child, child, leader);
    f.ids(member, child, leader);
    CHECK(f.scheduler.exit_process(leader, 0));
    CHECK(f.process(child).parent_pid() == 1);
    f.ids(child, child, leader);
    CHECK(f.scheduler.reap_process(leader));
    // Reaping compacts PCBs but neither session nor group is a PCB pointer.
    f.ids(child, child, leader);
    f.ids(member, child, leader);
    CHECK(f.scheduler.reparent_children(child, 1));
    CHECK(f.process(member).parent_pid() == 1);
    f.ids(member, child, leader);
    CHECK(f.scheduler.exit_process(child, 0));
    CHECK(f.scheduler.reap_process(child));
    f.ids(member, child, leader);
    const auto descendant = f.fork(member);
    f.ids(descendant, child, leader);
    CHECK(f.scheduler.set_process_group(member, descendant, 0));
    f.ids(descendant, descendant, leader);
}

// Controlling terminals by derivation (docs/modules/tty.md sections 4 and
// 9.7) on a ptmx-style pair: a session leader acquires the slave, fork keeps
// it and setsid drops it; a member's TIOCNOTTY detaches only that member (a
// fork inherits the detachment, setsid clears it); the leader's TIOCNOTTY
// queues SIGHUP then SIGCONT for the foreground group, its exit SIGHUP alone,
// and the master's last close SIGHUP then SIGCONT for the session leader.
void controlling_terminal_derivation()
{
    Fixture f;
    auto &s = f.scheduler;
    const auto leader = f.fork();
    CHECK(s.create_session(leader));
    const auto open = [&](auto bind) {
        const auto made = s.allocate_description({.readable = true, .writable = true, .flags = 2u});
        CHECK(made);
        CHECK(s.retain_description(made.value()));
        CHECK(bind(made.value()));
        return made.value();
    };
    private_pty::Handle master{};
    const auto master_id = open([&](u16 id) {
        const auto bound = s.bind_pty_multiplexer(id);
        if (bound) master = bound.value();
        return bound.ok();
    });
    CHECK(s.set_pty_lock(master_id, false));
    const private_pty::Handle pts{master.slot, master.generation, private_pty::Side::slave};
    const auto slave_id = open([&](u16 id) { return s.bind_pty_slave(id, pts).ok(); });
    const TerminalRef ref{static_cast<u16>(master.slot + 1), master.generation};
    CHECK(!s.controlling_terminal(f.process(leader)));
    CHECK(s.acquire_terminal(ref, f.process(leader), true, AcquireMode::open));
    CHECK(s.controlling_terminal(f.process(leader)) == ref);
    const auto member = f.fork(leader);
    CHECK(s.controlling_terminal(f.process(member)) == ref);
    const auto front = s.terminal_job_facts(member, ref);
    CHECK(front.is_ctty && front.has_foreground && front.in_foreground && front.pgrp == leader);
    const auto job = f.fork(leader);
    CHECK(s.set_process_group(leader, static_cast<i32>(job), static_cast<i32>(job)));
    const auto back = s.terminal_job_facts(job, ref);
    CHECK(back.is_ctty && back.has_foreground && !back.in_foreground && back.pgrp == job);
    const auto other = f.fork(leader);
    CHECK(s.create_session(other));
    CHECK(!s.controlling_terminal(f.process(other)) && !s.terminal_job_facts(other, ref).is_ctty);
    // Requests of the running caller, as the ioctl dispatch makes them.
    const auto request = [&](ProcessId pid, ConsoleControlOperation operation, i32 scalar = 0) -> Status {
        f.process(pid).threads()[0].native_active = true;
        CHECK(s.activate(pid));
        const auto subject = s.terminal_subject(slave_id);
        CHECK(subject);
        if (auto status = s.terminal_check(subject.value(), operation); !status) return status;
        TerminalRequest value{};
        value.operation = operation;
        value.scalar = scalar;
        const auto reply = s.terminal_control(subject.value(), value);
        return reply ? Status{} : reply.status();
    };
    using Op = ConsoleControlOperation;
    CHECK(request(member, Op::detach));
    CHECK(f.process(member).ctty_detached() && !s.controlling_terminal(f.process(member)));
    CHECK(request(member, Op::detach).code() == StatusCode::not_terminal);
    const auto heir = f.fork(member);
    CHECK(f.process(heir).ctty_detached() && !s.controlling_terminal(f.process(heir)));
    CHECK(s.create_session(heir) && !f.process(heir).ctty_detached());
    CHECK(s.controlling_terminal(f.process(leader)) == ref && s.controlling_terminal(f.process(job)) == ref);
    auto *terminal = s.terminal(ref);
    CHECK(terminal && !terminal->outbox_count);
    CHECK(request(leader, Op::set_foreground, static_cast<i32>(job)) && terminal->foreground == job);
    CHECK(request(leader, Op::detach));
    CHECK(terminal->outbox_count == 2 && terminal->outbox[0] == (TerminalOutboxEntry{job, tty::TerminalSignal::hup, true}) &&
          terminal->outbox[1] == (TerminalOutboxEntry{job, tty::TerminalSignal::cont, true}));
    CHECK(!terminal->session && !terminal->foreground);
    CHECK(!s.controlling_terminal(f.process(leader)) && !s.controlling_terminal(f.process(job)));
    while (terminal->outbox_count) static_cast<void>(terminal->take_outbox());
    // A re-acquire gives members forked earlier the terminal again (tty.md
    // section 14, item 12); a member that detached itself stays detached.
    CHECK(s.acquire_terminal(ref, f.process(leader), true, AcquireMode::ioctl));
    CHECK(s.controlling_terminal(f.process(job)) == ref && !s.controlling_terminal(f.process(member)));
    CHECK(request(leader, Op::set_foreground, static_cast<i32>(job)));
    CHECK(s.exit_process(leader, 0));
    CHECK(terminal->outbox_count == 1 && terminal->outbox[0] == (TerminalOutboxEntry{job, tty::TerminalSignal::hup, true}));
    CHECK(!terminal->session && !s.controlling_terminal(f.process(job)));
    static_cast<void>(terminal->take_outbox());
    CHECK(s.acquire_terminal(ref, f.process(heir), true, AcquireMode::open));
    CHECK(s.release_description(master_id));
    CHECK(terminal->outbox_count == 2 && terminal->outbox[0] == (TerminalOutboxEntry{heir, tty::TerminalSignal::hup, false}) &&
          terminal->outbox[1] == (TerminalOutboxEntry{heir, tty::TerminalSignal::cont, false}));
    CHECK(!terminal->session && !s.controlling_terminal(f.process(heir)));
    while (terminal->outbox_count) static_cast<void>(terminal->take_outbox());
    CHECK(s.release_description(slave_id));
}
} // namespace

int main()
{
    initial_membership_and_fork();
    validation_order_and_parent_authority();
    successful_exec_marker_and_fork_reset();
    thread_and_retained_leader_identities();
    group_membership_reserves_identity_until_last_reap();
    departed_leaders_reparent_and_compaction();
    controlling_terminal_derivation();
    std::printf("PASS process session integration checks=%zu guest=0 tty=1 smp=0\n", checks);
}
