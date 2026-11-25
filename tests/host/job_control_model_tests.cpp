// Hosted job-control model: signal numbering, default actions, CLD_STOPPED/
// CLD_CONTINUED value types, the actual scheduler stop overlay and the
// orphaned-process-group rule. No runtime delivery, guest or hardware evidence.
#include "ok/sched/process.hpp"
#include "ok/user/job_control_signals.hpp"
#include "ok/user/native_signal_action.hpp"
#include "ok/user/native_signal_info.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>

using namespace ok;
namespace job = ok::user::job;
using Profile = abi::linux_user::Profile;
namespace
{
usize checks{};
usize orphan_cases{};
#define CHECK(condition) do { ++checks; if (!static_cast<bool>(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

// Linux asm-generic/signal.h and arch/mips/include/uapi/asm/signal.h, by name.
constexpr std::array<std::string_view, 32> generic_names{"", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS",
    "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN",
    "TTOU", "URG", "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS"};
constexpr std::array<std::string_view, 32> mips_names{"", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "EMT", "FPE",
    "KILL", "BUS", "SEGV", "SYS", "PIPE", "ALRM", "TERM", "USR1", "USR2", "CHLD", "PWR", "WINCH", "URG", "IO", "STOP",
    "TSTP", "CONT", "TTIN", "TTOU", "VTALRM", "PROF", "XCPU", "XFSZ"};

u32 named(const std::array<std::string_view, 32> &names, std::string_view name)
{
    for (u32 signal = 1; signal < 32; ++signal)
        if (names[signal] == name) return signal;
    return 0;
}

void numbering(const job::Numbering &n, const std::array<std::string_view, 32> &names, u32 maximum)
{
    CHECK(n.max == maximum);
    CHECK(n.hup == named(names, "HUP") && n.interrupt == named(names, "INT") && n.quit == named(names, "QUIT"));
    CHECK(n.kill == named(names, "KILL") && n.term == named(names, "TERM") && n.child == named(names, "CHLD"));
    CHECK(n.cont == named(names, "CONT") && n.stop == named(names, "STOP") && n.tstp == named(names, "TSTP"));
    CHECK(n.ttin == named(names, "TTIN") && n.ttou == named(names, "TTOU") && n.window == named(names, "WINCH"));
    CHECK(n.urgent == named(names, "URG"));
    CHECK(n.to_generic[0] == 0);
    for (u32 signal = 1; signal < 32; ++signal)
    {
        // The generic counterpart is the generic signal of the same name.
        CHECK(n.to_generic[signal] == named(generic_names, names[signal]));
        for (u32 other = signal + 1; other < 32; ++other)
            CHECK(!n.to_generic[signal] || n.to_generic[signal] != n.to_generic[other]);
        u32 expected_stops = 0;
        for (auto name : {"STOP", "TSTP", "TTIN", "TTOU"}) expected_stops += names[signal] == name;
        CHECK(job::stop_family(n, signal) == (expected_stops == 1));
        const auto action = job::default_action(n, signal);
        if (names[signal] == "CHLD" || names[signal] == "WINCH" || names[signal] == "URG")
            CHECK(action == job::DefaultAction::ignore);
        else if (names[signal] == "CONT") CHECK(action == job::DefaultAction::cont);
        else if (expected_stops) CHECK(action == job::DefaultAction::stop);
        else CHECK(action == job::DefaultAction::terminate);
    }
    for (u32 signal = 32; signal <= maximum + 1; ++signal)
    {
        CHECK(!job::stop_family(n, signal));
        CHECK(job::default_action(n, signal) == job::DefaultAction::terminate);
    }
}

void translation()
{
    numbering(job::generic, generic_names, 64);
    numbering(job::mips, mips_names, 128);
    CHECK(&job::native == &job::generic);
    const std::array<const job::Numbering *, 2> all{&job::generic, &job::mips};
    const std::array<const std::array<std::string_view, 32> *, 2> names{&generic_names, &mips_names};
    for (usize from = 0; from < all.size(); ++from)
        for (usize to = 0; to < all.size(); ++to)
        {
            const auto &source = *all[from];
            const auto &target = *all[to];
            CHECK(!job::translate(source, target, 0) && !job::translate(source, target, source.max + 1));
            for (u32 signal = 1; signal <= source.max; ++signal)
            {
                const auto mapped = job::translate(source, target, signal);
                if (signal >= 32)
                {
                    CHECK(mapped == (signal <= target.max ? signal : 0));
                    continue;
                }
                // Same name on both sides, or no counterpart at all.
                const auto expected = named(*names[to], (*names[from])[signal]);
                CHECK(mapped == expected);
                if (mapped) CHECK(job::translate(target, source, mapped) == signal);
                if (from == to) CHECK(mapped == signal);
            }
        }
    // EMT has no generic counterpart; STKFLT has none on MIPS.
    CHECK(!job::translate(job::mips, job::generic, 7) && !job::translate(job::generic, job::mips, 16));
    CHECK(job::translate(job::generic, job::mips, 20) == 24 && job::translate(job::mips, job::generic, 25) == 18);
    static_assert(job::translate(job::generic, job::mips, 19) == 23);
    static_assert(&job::linux_numbering<Profile::mips> == &job::mips &&
                  &job::linux_numbering<Profile::mips64> == &job::mips &&
                  &job::linux_numbering<Profile::ppc> == &job::generic);
}

// Per Linux profile: codec constants, stop classification, the interrupting
// predicate the wait selectors use, and codes 5/6 with the SI_KERNEL set.
template <Profile P>
void profile()
{
    using Actions = user::native::SignalActionCodec<P>;
    using Info = user::native::SignalInfoCodec<P>;
    using Action = sched::SignalAction;
    const auto &n = job::linux_numbering<P>;
    constexpr bool mips = P == Profile::mips || P == Profile::mips64;
    CHECK(&n == (mips ? &job::mips : &job::generic));
    CHECK(Actions::max_signal == n.max && Info::maximum_signal == n.max);
    CHECK(Actions::stop_signal == n.stop && Actions::tstp_signal == n.tstp && Actions::ttin_signal == n.ttin &&
          Actions::ttou_signal == n.ttou && Actions::continue_signal == n.cont && Actions::child_signal == n.child &&
          Actions::hangup_signal == n.hup && Actions::window_signal == n.window && Actions::urgent_signal == n.urgent);
    const Action defaulted{}, ignored{Action::ignored_handler}, caught{0x1000};
    for (u32 signal = 1; signal <= n.max; ++signal)
    {
        const auto action = job::default_action(n, signal);
        CHECK(Actions::default_stop(signal) == (action == job::DefaultAction::stop));
        CHECK(Actions::stop_or_continue(signal) ==
              (action == job::DefaultAction::stop || action == job::DefaultAction::cont));
        CHECK(Actions::interrupts(signal, caught));
        CHECK(!Actions::interrupts(signal, ignored));
        // A default stop or a default-ignored signal (CONT included) leaves a wait armed.
        CHECK(Actions::interrupts(signal, defaulted) == (action == job::DefaultAction::terminate));
        const sched::PendingSignalInfo kernel{signal, 128, 0, 0};
        const bool terminal = signal == n.hup || signal == n.interrupt || signal == n.quit || signal == n.tstp ||
            signal == n.ttin || signal == n.ttou || signal == n.cont || signal == n.window;
        CHECK(Info::kernel_terminal_signal(signal) == terminal);
        CHECK(Info::template frame_valid<>(kernel) == (terminal || signal == 14));
    }
    for (u32 signal : {n.stop, n.tstp, n.ttin, n.ttou, n.cont, n.max})
    {
        const auto stopped = sched::ChildExitStatus::stopped(signal);
        const auto continued = sched::ChildExitStatus::continued(signal);
        CHECK(stopped.valid(n.max) && continued.valid(n.max));
        CHECK(stopped.code() == 5 && continued.code() == 6 && stopped.status() == static_cast<i32>(signal));
        const sched::PendingSignalInfo killed{n.child, 2, 41, 42, static_cast<i32>(signal)};
        for (i32 code : {5, 6})
        {
            sched::PendingSignalInfo report = killed;
            report.code = code;
            CHECK(Info::valid(report) && Info::template frame_valid<>(report));
            // si_status is written exactly as for CLD_KILLED; only si_code differs.
            std::array<std::byte, 128> left{}, right{};
            CHECK(Info::encode(report, left) && Info::encode(killed, right));
            for (usize at = 0; at < 128; ++at)
                if (at < Info::code_offset || at >= Info::code_offset + 4) CHECK(left[at] == right[at]);
            CHECK(left != right);
            report.signal = n.child + 1;
            CHECK(!Info::valid(report));
        }
    }
    CHECK(!sched::ChildExitStatus::stopped(n.max + 1).valid(n.max) && !sched::ChildExitStatus::continued(0).valid(n.max));
}

// Brute-force orphan oracle over every parent, group and session assignment
// of five processes (pids 1..5, pid 1 is init). A group is a set partition
// with the smallest pid as its id; a session is a union of groups.
struct Assignment
{
    std::array<u64, 6> parent{}, group{}, session{};
    std::array<bool, 6> live{};
};

// POSIX 3.266 with Linux's two exclusions: a member whose parent is init, and
// the `ignored` (exiting) process, never keep the group attached.
bool oracle_orphaned(const Assignment &a, u64 group, u64 ignored)
{
    for (u64 member = 1; member <= 5; ++member)
    {
        if (a.group[member] != group || !a.live[member] || member == ignored) continue;
        const u64 parent = a.parent[member];
        if (!parent || parent == sched::user_init_pid) continue;
        if (a.group[parent] != group && a.session[parent] == a.session[member]) return false;
    }
    return true;
}

template <usize N> void partitions(std::array<u8, N> &block, usize at, u8 used, auto &&visit)
{
    if (at == N) { visit(block, used); return; }
    for (u8 b = 0; b <= used && b < N; ++b)
    {
        block[at] = b;
        partitions(block, at + 1, b == used ? static_cast<u8>(used + 1) : used, visit);
    }
}

void orphan_oracle()
{
    std::array<u8, 5> group_block{};
    usize assignments = 0;
    partitions(group_block, 0, 0, [&](const std::array<u8, 5> &groups, u8 group_count) {
        std::array<u8, 5> session_block{};
        // A session partition over the groups.
        auto sessions = [&](auto &&self, usize at, u8 used) -> void {
            if (at < group_count)
            {
                for (u8 b = 0; b <= used && b < group_count; ++b)
                {
                    session_block[at] = b;
                    self(self, at + 1, b == used ? static_cast<u8>(used + 1) : used);
                }
                return;
            }
            Assignment a;
            std::array<u64, 5> group_id{}, session_id{};
            for (u64 pid = 5; pid >= 1; --pid)
            {
                group_id[groups[pid - 1]] = pid;
                session_id[session_block[groups[pid - 1]]] = pid;
            }
            for (u64 pid = 1; pid <= 5; ++pid)
            {
                a.group[pid] = group_id[groups[pid - 1]];
                a.session[pid] = session_id[session_block[groups[pid - 1]]];
            }
            std::array<u8, 5> parent_digit{};
            for (u32 parents = 0; parents < 7776; ++parents)
            {
                u32 rest = parents;
                bool self_parent = false;
                for (u64 pid = 1; pid <= 5; ++pid)
                {
                    parent_digit[pid - 1] = static_cast<u8>(rest % 6);
                    rest /= 6;
                    a.parent[pid] = parent_digit[pid - 1];
                    self_parent = self_parent || a.parent[pid] == pid;
                }
                if (self_parent) continue;
                ++assignments;
                // Liveness varies with the counter: on every third assignment a pid is dead when its bit is set.
                for (u64 pid = 1; pid <= 5; ++pid) a.live[pid] = !((assignments >> (pid - 1)) & 1U) || assignments % 3;
                std::array<sched::JobMember, 5> members{};
                for (u64 pid = 1; pid <= 5; ++pid)
                    members[pid - 1] = {pid, a.parent[pid], a.group[pid], a.session[pid], a.live[pid]};
                for (u64 group = 1; group <= 6; ++group)
                    for (u64 ignored = 0; ignored <= 5; ++ignored)
                    {
                        const bool model = sched::job_group_orphaned(members, group, ignored, sched::user_init_pid,
                                                                     [](const sched::JobMember &m) { return m; });
                        if (model != oracle_orphaned(a, group, ignored))
                        {
                            std::fprintf(stderr, "orphan mismatch group=%llu ignored=%llu\n",
                                         static_cast<unsigned long long>(group),
                                         static_cast<unsigned long long>(ignored));
                            std::abort();
                        }
                        ++orphan_cases;
                    }
            }
        };
        sessions(sessions, 0, 0);
    });
    // 358 group/session pairs x 5^5 parent vectors without self-parenting.
    CHECK(assignments == 358U * 3125U);
    static_assert(sched::job_group_orphaned(std::array<sched::JobMember, 2>{
        sched::JobMember{2, 3, 2, 1, true}, sched::JobMember{3, 1, 3, 1, true}}, 2, 0, 1,
        [](const sched::JobMember &m) { return m; }) == false);
}

struct Fixture
{
    std::unique_ptr<sched::Scheduler> owned{std::make_unique<sched::Scheduler>()};
    sched::Scheduler &scheduler{*owned};
    arch::CpuContext context{arch::configured_arch_operations().make_user_context({0x400000, 0x800000})};
    Fixture()
    {
        auto init = scheduler.create_init_process(context);
        CHECK(init && init.value() == sched::user_init_pid);
        CHECK(scheduler.set_runnable(init.value()));
    }
    sched::ProcessId fork(sched::ProcessId parent)
    {
        auto child = scheduler.fork_process(parent);
        CHECK(child);
        return child.value();
    }
    sched::ProcessControlBlock &process(sched::ProcessId pid)
    {
        auto *process = scheduler.find(pid);
        CHECK(process);
        return *process;
    }
    // The scheduler's answer against the oracle over its actual PCBs.
    void orphan_agrees(sched::ProcessId group, sched::ProcessId ignored = 0)
    {
        bool attached = false;
        for (const auto &member : scheduler.processes())
        {
            if (member.process_group_id() != group || member.pid() == ignored ||
                member.state() == sched::ProcessState::zombie || member.state() == sched::ProcessState::exited ||
                member.parent_pid() == sched::user_init_pid) continue;
            const auto *parent = scheduler.find(member.parent_pid());
            attached = attached || (parent && parent->process_group_id() != group &&
                                    parent->session_id() == member.session_id());
        }
        CHECK(scheduler.process_group_orphaned(group, ignored) == !attached);
        ++orphan_cases;
    }
};

void overlay()
{
    Fixture f;
    auto &s = f.scheduler;
    const auto shell = f.fork(sched::user_init_pid);
    CHECK(s.create_session(shell));
    const auto job = f.fork(shell);
    CHECK(s.set_process_group(job, 0, 0));
    const auto peer = f.fork(job);
    CHECK(f.process(peer).process_group_id() == job && f.process(job).session_id() == shell);
    // The shell's own group hangs off init; the job's group hangs off the shell.
    f.orphan_agrees(shell);
    f.orphan_agrees(job);
    f.orphan_agrees(job, job);
    CHECK(s.process_group_orphaned(shell) && !s.process_group_orphaned(job));
    // Ignoring the leader leaves only the peer, whose parent is inside the group.
    CHECK(s.process_group_orphaned(job, job));
    CHECK(sched::SchedulerState::child_group_wait_key(job) == ((u64{1} << 32) | job));

    const sched::JobControlState clean{};
    CHECK(f.process(job).job_control() == clean && s.stopped_processes() == 0);
    CHECK(s.stop_process(999, 19).code() == StatusCode::not_found);
    CHECK(s.stop_process(job, 0).code() == StatusCode::invalid_argument);
    CHECK(!s.continue_process(job) && !s.continue_process(999));

    // A parent blocked in a group-selector wait is woken by the stop.
    CHECK(s.set_runnable(shell));
    CHECK(s.activate_thread(shell, shell));
    auto ticket = s.register_current_wait(sched::ThreadWaitReason::child,
                                          sched::SchedulerState::child_group_wait_key(job));
    CHECK(ticket);
    auto *waiter = s.find_thread(shell, shell);
    CHECK(waiter && waiter->state == sched::ProcessState::blocked);
    CHECK(s.set_runnable(job));
    CHECK(s.stop_process(job, 19));
    CHECK(waiter->state != sched::ProcessState::blocked &&
          waiter->wait_registration.completion() == sched::WaitCompletion::ready);
    CHECK((f.process(job).job_control() == sched::JobControlState{19, true, true, false, 0}));
    CHECK(s.stopped_processes() == 1 && s.process_group_has_stopped(job) && !s.process_group_has_stopped(shell));
    // Stopping twice changes nothing; the thread state is never touched.
    const auto thread_state = f.process(job).leader().state;
    CHECK(s.stop_process(job, 20));
    CHECK((f.process(job).job_control() == sched::JobControlState{19, true, true, false, 0}));
    CHECK(s.stopped_processes() == 1 && f.process(job).leader().state == thread_state);
    CHECK(f.process(job).state() == sched::ProcessState::runnable);
    // Never selected or activated while stopped.
    CHECK(!s.activate_thread(job, job));
    for (int round = 0; round < 8; ++round)
    {
        const auto next = s.schedule_next();
        CHECK(!next || next.value() != job);
    }
    s.consume_job_report(job, true);
    CHECK((f.process(job).job_control() == sched::JobControlState{19, true, false, false, 0}));

    CHECK(s.continue_process(job));
    CHECK((f.process(job).job_control() == sched::JobControlState{19, false, false, true, 1}));
    CHECK(s.stopped_processes() == 0 && !s.process_group_has_stopped(job));
    CHECK(!s.continue_process(job));
    CHECK(f.process(job).job_control().continue_generation == 1);
    CHECK(s.activate_thread(job, job));
    s.consume_job_report(job, false);
    CHECK((f.process(job).job_control() == sched::JobControlState{19, false, false, false, 1}));
    s.consume_job_report(999, true);

    // A new stop clears an unreported continue; a fork starts clean.
    CHECK(s.continue_process(job) == false);
    CHECK(s.stop_process(peer, 21) && s.stop_process(job, 22));
    CHECK(s.stopped_processes() == 2 && f.process(job).job_control().stop_signal == 22);
    const auto forked = f.fork(shell);
    CHECK(f.process(forked).job_control() == clean);
    CHECK(s.continue_process(peer));
    CHECK((f.process(peer).job_control() == sched::JobControlState{21, false, false, true, 1}));
    CHECK(s.stop_process(peer, 20));
    CHECK((f.process(peer).job_control() == sched::JobControlState{20, true, true, false, 1}));

    // Exit of a stopped process drops its count and overlay and wakes the group key.
    CHECK(s.activate_thread(shell, shell));
    ticket = s.register_current_wait(sched::ThreadWaitReason::child, sched::SchedulerState::child_group_wait_key(job));
    CHECK(ticket);
    CHECK(s.exit_process(job, 0));
    CHECK(waiter->wait_registration.completion() == sched::WaitCompletion::ready);
    CHECK(f.process(job).job_control() == clean && s.stopped_processes() == 1);
    CHECK(s.stop_process(job, 19).code() == StatusCode::already_exists && !s.continue_process(job));
    // The exiting member is gone from the group; the remaining member still has the shell.
    f.orphan_agrees(job);
    CHECK(s.exit_process(peer, 0));
    CHECK(s.stopped_processes() == 0);
    f.orphan_agrees(job);

    // Linux: a group whose only outside parent is init is orphaned.
    const auto daemon = f.fork(sched::user_init_pid);
    CHECK(s.set_process_group(daemon, 0, 0));
    f.orphan_agrees(daemon);
    CHECK(s.process_group_orphaned(daemon));
    // A child in a different session never attaches the group.
    const auto other = f.fork(shell);
    CHECK(s.set_process_group(other, 0, 0));
    const auto grandchild = f.fork(other);
    CHECK(s.create_session(grandchild));
    f.orphan_agrees(other);
    f.orphan_agrees(grandchild);
    CHECK(!s.process_group_orphaned(other) && s.process_group_orphaned(grandchild));
    f.orphan_agrees(other, other);
}
} // namespace

int main()
{
    translation();
    profile<Profile::i386>();
    profile<Profile::x86_64>();
    profile<Profile::aarch64>();
    profile<Profile::arm32>();
    profile<Profile::rv64>();
    profile<Profile::rv32>();
    profile<Profile::loongarch64>();
    profile<Profile::mips>();
    profile<Profile::mips64>();
    profile<Profile::ppc>();
    orphan_oracle();
    overlay();
    std::printf("job control model checks=%zu\n", checks);
    std::printf("JOB_CONTROL_MODEL_PASS profiles=10 numberings=2 orphan_cases=%zu guest=0\n", orphan_cases);
}
