// The orphaned-group stop drop on the configured profile's Linux numbering:
// TSTP/TTIN/TTOU to an orphaned group are dropped, SIGSTOP never is. The drop
// compares in the recipient's numbering, so a native SIGSTOP (19) stops a
// native process under MIPS Linux numbering (SIGSTOP 23). Built per profile
// with hosted backends. Hosted evidence only; no guest execution.
#define OK_NATIVE_SIGNAL_RUNTIME 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_child_exit_runtime_fixture.hpp"

namespace
{
using N = syscall::Number;
constexpr auto profile = lu::configured_native_profile();
using Codec = lu::Codec<profile>;
constexpr const auto &linux_numbers = user::job::linux_numbering<profile>;
constexpr uptr status_at = hw::user_begin + 384;

u64 negative(u64 group) { return static_cast<u64>(-static_cast<i64>(group)); }
i64 native_call(Kernel &kernel, u64 pid, N number, std::array<uptr, 6> arguments,
                std::source_location at = std::source_location::current())
{
    activate(kernel, pid, pid);
    bool complete = true;
    const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), arguments}, complete);
    expect(complete, at);
    return result;
}
i64 native_kill(Kernel &kernel, u64 sender, u64 selector, u64 signal)
{ return native_call(kernel, sender, N::kill, {selector, signal}); }
// Linux kill(2) from a Linux-personality sender.
i64 linux_kill(Kernel &kernel, u64 sender, u64 selector, u64 signal,
               std::source_location at = std::source_location::current())
{
    activate(kernel, sender, sender);
    auto &thread = *kernel.scheduler().find_thread(sender, sender);
    const auto saved = thread.native_frame;
    // The profile's Linux number in the hosted trap decoder's slots, which are
    // the x86_64 ones on every profile (native_clock_model.hpp).
    auto &registers = thread.native_frame.registers;
    registers[14] = *Codec::syscall_number(lu::Operation::kill);
    registers[9] = selector; registers[10] = signal;
    bool complete = true;
    i64 result = -999;
    expect(native::private_signal_call(kernel, *kernel.scheduler().find(sender), thread, complete, result), at);
    thread.native_frame = saved;
    expect(complete, at);
    return result;
}
const sched::JobControlState &job(Kernel &kernel, u64 pid) { return kernel.scheduler().find(pid)->job_control(); }
bool stopped_by(Kernel &kernel, u64 pid, u32 signal)
{ return job(kernel, pid).stopped && job(kernel, pid).stop_signal == signal; }
bool pending(u64 pid, u32 signal) { return native::private_signal_find(pid)->shared.pending_mask().contains(signal); }
void drain(u64 pid)
{
    native::PrivateProcessQueue::Delivery ticket;
    auto &queue = native::private_signal_find(pid)->shared;
    while (queue.peek({}, ticket)) CHECK(queue.consume(ticket));
}
u64 create_native(Kernel &kernel)
{
    const auto pid = create(kernel);
    kernel.scheduler().find(pid)->resources().personality = sched::UserPersonality::native;
    return pid;
}

// A lone native process is an orphaned group: it stops itself with SIGSTOP,
// while its own TSTP/TTIN/TTOU are dropped.
void native_self(Kernel &kernel)
{
    const auto self = create_native(kernel), peer = create_native(kernel);
    expect(native::private_group_orphaned(kernel, self));
    for (u32 signal : {u32{OK_SIGTSTP}, u32{OK_SIGTTIN}, u32{OK_SIGTTOU}})
        expect(native_kill(kernel, self, self, signal) == 0 && !job(kernel, self).stopped);
    expect(native_kill(kernel, self, self, OK_SIGSTOP) == 0 && stopped_by(kernel, self, OK_SIGSTOP));
    expect(kernel.scheduler().stopped_processes() == 1);
    expect(native_kill(kernel, peer, self, OK_SIGCONT) == 0 && !job(kernel, self).stopped);
    release(kernel, peer); release(kernel, self);
}

// A child that left its parent's session leads an orphaned group: the
// parent's SIGSTOP, directed or to the group, stops it and WUNTRACED sees it;
// TSTP/TTIN/TTOU from the parent are dropped.
void native_peer(Kernel &kernel)
{
    const auto parent = create_native(kernel);
    const auto child = fork(kernel, parent);
    kernel.scheduler().find(child)->resources().personality = sched::UserPersonality::native;
    expect(native_call(kernel, child, N::setsid, {}) == static_cast<i64>(child));
    expect(native::private_group_orphaned(kernel, child));
    for (u32 signal : {u32{OK_SIGTSTP}, u32{OK_SIGTTIN}, u32{OK_SIGTTOU}})
    {
        expect(native_kill(kernel, parent, child, signal) == 0 && !job(kernel, child).stopped);
        expect(native_kill(kernel, parent, negative(child), signal) == 0 && !job(kernel, child).stopped);
    }
    expect(native_call(kernel, parent, N::wait4, {child, status_at, OK_WNOHANG | OK_WUNTRACED, 0}) == 0);
    expect(native_kill(kernel, parent, child, OK_SIGSTOP) == 0 && stopped_by(kernel, child, OK_SIGSTOP));
    expect(job(kernel, child).stop_report);
    expect(native_call(kernel, parent, N::wait4, {child, status_at, OK_WNOHANG | OK_WUNTRACED, 0}) ==
           static_cast<i64>(child) && !job(kernel, child).stop_report);
    expect(native_kill(kernel, parent, negative(child), OK_SIGCONT) == 0 && !job(kernel, child).stopped);
    expect(native_kill(kernel, parent, negative(child), OK_SIGSTOP) == 0 && stopped_by(kernel, child, OK_SIGSTOP));
    expect(native_kill(kernel, parent, child, OK_SIGCONT) == 0 && !job(kernel, child).stopped);
    drain(parent);
    release(kernel, child); release(kernel, parent);
}

// A Linux sender reaches a native recipient in its own numbering: its SIGSTOP
// translates to native 19 and stops, its SIGTSTP is dropped.
void linux_to_native(Kernel &kernel)
{
    const auto sender = create(kernel), target = create_native(kernel);
    for (u32 signal : {linux_numbers.tstp, linux_numbers.ttin, linux_numbers.ttou})
        expect(linux_kill(kernel, sender, target, signal) == 0 && !job(kernel, target).stopped);
    expect(linux_kill(kernel, sender, target, linux_numbers.stop) == 0 && stopped_by(kernel, target, OK_SIGSTOP));
    expect(linux_kill(kernel, sender, target, linux_numbers.cont) == 0 && !job(kernel, target).stopped);
    release(kernel, target); release(kernel, sender);
}

// Linux-personality recipients keep Linux numbering: SIGSTOP (23 on MIPS)
// stops and TSTP/TTIN/TTOU are dropped at generation, from a Linux sender, the
// recipient itself or a native sender whose numbers are translated first. (The
// drop at dequeue is covered on x86_64 by native_job_control_tests.cpp; MIPS
// delivery needs a frame lease this fixture does not set up.)
void linux_recipient(Kernel &kernel)
{
    const auto sender = create(kernel), target = create(kernel), native_sender = create_native(kernel);
    expect(native::private_group_orphaned(kernel, target));
    for (u32 signal : {linux_numbers.tstp, linux_numbers.ttin, linux_numbers.ttou})
    {
        expect(linux_kill(kernel, sender, target, signal) == 0 && !job(kernel, target).stopped);
        expect(linux_kill(kernel, target, target, signal) == 0 && !job(kernel, target).stopped);
        expect(!pending(target, signal));
    }
    for (u32 signal : {u32{OK_SIGTSTP}, u32{OK_SIGTTIN}, u32{OK_SIGTTOU}})
        expect(native_kill(kernel, native_sender, target, signal) == 0 && !job(kernel, target).stopped);
    expect(linux_kill(kernel, sender, target, linux_numbers.stop) == 0 && stopped_by(kernel, target, linux_numbers.stop));
    expect(linux_kill(kernel, sender, target, linux_numbers.cont) == 0 && !job(kernel, target).stopped);
    expect(native_kill(kernel, native_sender, target, OK_SIGSTOP) == 0 && stopped_by(kernel, target, linux_numbers.stop));
    expect(native_kill(kernel, native_sender, target, OK_SIGCONT) == 0 && !job(kernel, target).stopped);
    drain(target);
    release(kernel, native_sender); release(kernel, target); release(kernel, sender);
}
} // namespace

int main()
{
    static_assert(!native::native_signal_diagnostics);
    static Kernel kernel; CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    const auto init = create_init(kernel);
    native_self(kernel);
    native_peer(kernel);
    linux_to_native(kernel);
    linux_recipient(kernel);
    expect(native::runtime_status().ok() && kernel.scheduler().stopped_processes() == 0);
    expect(kernel.scheduler().find(init)->state() != sched::ProcessState::zombie);
    std::printf("NATIVE_JOB_CONTROL_ORPHAN_STOP_PASS native_stop=%u linux_stop=%u checks=%zu hosted=1 guest=0\n",
                u32{OK_SIGSTOP}, linux_numbers.stop, checks);
}
