// Linux interval timers through the actual private runtime with hosted copies
// and a model clock. Model coverage only: no guest, interrupt or hardware timer.
#define OK_ENABLE_TEST_POINTS 1
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>

namespace
{
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "LINUX_TIMERS_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr u64 ms = 1'000'000;
constexpr uptr site = hw::user_begin + 1024, handler = hw::user_begin + 2048, scratch = hw::user_begin + 128;
constexpr uptr spec = scratch + 256, old_spec = scratch + 320, event = scratch + 384, id_out = scratch + 512;
using Op = lu::Operation;
struct Group { sched::ProcessId pid{}; sched::ThreadId peer{}; };
sched::ThreadControlBlock &thread(Kernel &kernel, u64 pid, u64 tid)
{ auto *t = kernel.scheduler().find_thread(pid, tid); CHECK(t); return *t; }
void reset_frame(sched::ThreadControlBlock &t)
{
    auto &f = t.native_frame; f = {}; f.flags = 0x202; f.sp = hw::user_begin + 8192; f.pc = site;
    const auto fp = native::X86SignalFrame::reset_fp();
    for (usize i = 0; i < fp.size(); ++i) f.fp_state[i] = static_cast<u8>(fp[i]);
}
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("linux-timers",
        kernel.arch().make_user_context({site, hw::user_begin + 8192})); CHECK(made);
    auto *p = kernel.scheduler().find(made.value());
    p->resources().personality = sched::UserPersonality::linux;
    p->resources().native_space = {0x100000 + p->pid() * 4096, static_cast<u32>(p->pid() % 16), 1};
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    const auto peer = kernel.scheduler().create_thread(p->pid(), p->threads()[0].context); CHECK(peer);
    for (auto &t : p->threads()) { CHECK(native::prepare_native_thread(t)); t.native_active = true; reset_frame(t); }
    CHECK(kernel.scheduler().set_runnable(p->pid()));
    native::runtime_kernel = &kernel; native::started = true;
    return {p->pid(), peer.value()};
}
void retire(Kernel &kernel, Group g)
{
    native::release_process(kernel, g.pid);
    if (auto *p = kernel.scheduler().find(g.pid); p && p->state() != sched::ProcessState::zombie)
        CHECK(kernel.scheduler().exit_process(g.pid, 0));
    CHECK(kernel.scheduler().reap_process(g.pid));
    expect(native::runtime_status().ok());
}
void action(Kernel &kernel, Group g, u32 signal, u64 disposition = handler)
{
    auto *a = native::private_signal_table(kernel, g.pid)->find(signal); CHECK(a);
    *a = {}; a->handler = disposition;
    if (disposition > 1) { a->flags = 0x04000004; a->restorer = handler + 4; }
}
native::PrivateProcessQueue &shared(Kernel &kernel, Group g)
{ auto *q = native::private_signal_shared(kernel, g.pid); CHECK(q); return *q; }
bool pending(Kernel &kernel, Group g, u32 signal) { return shared(kernel, g).pending_mask().contains(signal); }
sched::PendingSignalInfo take(Kernel &kernel, Group g)
{
    native::PrivateProcessQueue::Delivery delivery;
    CHECK(shared(kernel, g).peek({}, delivery) && shared(kernel, g).consume(delivery));
    return delivery.info;
}
u64 get(uptr address, usize bytes = 8)
{
    u64 value{};
    for (usize i = 0; i < bytes; ++i) value |= static_cast<u64>(mock::memory[address - hw::user_begin + i]) << (8 * i);
    return value;
}
void put(uptr address, u64 value, usize bytes = 8)
{
    for (usize i = 0; i < bytes; ++i) mock::memory[address - hw::user_begin + i] = static_cast<std::byte>(value >> (8 * i));
}
// struct itimerval / itimerspec: interval, then value.
void itimerval(uptr at, u64 interval_us, u64 value_us)
{
    put(at, interval_us / 1000000); put(at + 8, interval_us % 1000000);
    put(at + 16, value_us / 1000000); put(at + 24, value_us % 1000000);
}
void itimerspec(uptr at, u64 interval, u64 value)
{
    put(at, interval / 1000000000); put(at + 8, interval % 1000000000);
    put(at + 16, value / 1000000000); put(at + 24, value % 1000000000);
}
u64 spec_interval(uptr at) { return get(at) * 1000000000 + get(at + 8); }
u64 spec_value(uptr at) { return get(at + 16) * 1000000000 + get(at + 24); }
void sigevent(u64 value, u32 signal, u32 notify, u32 tid = 0)
{
    for (usize i = 0; i < 64; ++i) mock::memory[event - hw::user_begin + i] = std::byte{};
    put(event, value); put(event + 8, signal, 4); put(event + 12, notify, 4); put(event + 16, tid, 4);
}
i64 call(Kernel &kernel, Group g, u64 tid, Op op, std::array<u64, 6> a)
{
    auto &t = thread(kernel, g.pid, tid); auto &f = t.native_frame;
    f.registers[14] = *lu::Codec<lu::Profile::x86_64>::syscall_number(op);
    f.registers[9] = a[0]; f.registers[10] = a[1]; f.registers[11] = a[2];
    f.registers[5] = a[3]; f.registers[7] = a[4]; f.registers[6] = a[5];
    i64 result = -999;
    expect(native::private_timer_call(kernel, *kernel.scheduler().find(g.pid), t, result));
    return result;
}
void service(Kernel &kernel, u64 at)
{
    mock::now = at;
    static_cast<void>(native::private_timer_service(kernel, at));
    expect(native::runtime_status().ok());
}

void itimer_real(Kernel &kernel)
{
    const auto g = create(kernel);
    mock::now = 1000 * ms;
    // ITIMER_VIRTUAL/PROF and malformed timevals change nothing.
    itimerval(scratch, 0, 30000);
    expect(call(kernel, g, g.pid, Op::setitimer, {1, scratch, 0}) == -22);
    expect(call(kernel, g, g.pid, Op::getitimer, {2, scratch + 64}) == -22);
    itimerval(scratch, 1000000, 30000); put(scratch + 8, 1000000);
    expect(call(kernel, g, g.pid, Op::setitimer, {0, scratch, 0}) == -22);
    expect(call(kernel, g, g.pid, Op::setitimer, {0, 8, 0}) == -14);
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 80) == 0 && get(scratch + 88) == 0);
    // One shot after 30 ms; the old setting reads back zero.
    itimerval(scratch, 0, 30000);
    expect(call(kernel, g, g.pid, Op::setitimer, {0, scratch, scratch + 64}) == 0 && get(scratch + 88) == 0);
    mock::now += 10 * ms;
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 88) == 20000);
    service(kernel, 1029 * ms); expect(!pending(kernel, g, 14));
    service(kernel, 1030 * ms); expect(pending(kernel, g, 14));
    expect(take(kernel, g) == sched::PendingSignalInfo{14, 128, 0, 0});
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 88) == 0);
    service(kernel, 1100 * ms); expect(!pending(kernel, g, 14));
    // An interval timer queues one SIGALRM while it stays blocked and rearms on
    // its grid only after the dequeue.
    for (auto &t : kernel.scheduler().find(g.pid)->threads()) t.native_signal_mask.set(14);
    mock::now = 2000 * ms; itimerval(scratch, 10000, 10000);
    expect(call(kernel, g, g.pid, Op::setitimer, {0, scratch, 0}) == 0);
    service(kernel, 2010 * ms); expect(pending(kernel, g, 14));
    service(kernel, 2045 * ms);
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 &&
           get(scratch + 72) == 10000 && get(scratch + 80) == 0 && get(scratch + 88) == 0);
    expect(take(kernel, g).signal == 14);
    service(kernel, 2046 * ms);
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 88) == 4000);
    service(kernel, 2050 * ms); expect(pending(kernel, g, 14) && take(kernel, g).signal == 14);
    // An ignored SIGALRM is dropped and leaves the timer inactive with its interval.
    action(kernel, g, 14, 1);
    for (auto &t : kernel.scheduler().find(g.pid)->threads()) t.native_signal_mask = {};
    service(kernel, 2051 * ms); service(kernel, 2060 * ms);
    expect(!pending(kernel, g, 14));
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 72) == 10000 && get(scratch + 88) == 0);
    service(kernel, 2100 * ms); expect(!pending(kernel, g, 14));
    // alarm rounds the old remainder up and shares the timer with setitimer.
    action(kernel, g, 14, 0);
    mock::now = 3000 * ms; expect(call(kernel, g, g.pid, Op::alarm, {5}) == 0);
    mock::now = 4200 * ms; expect(call(kernel, g, g.pid, Op::alarm, {2}) == 4);
    mock::now = 4700 * ms; expect(call(kernel, g, g.pid, Op::alarm, {0}) == 2);
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 80) == 0 && get(scratch + 88) == 0);
    // An expiry not served yet still reads as one microsecond.
    mock::now = 5000 * ms; itimerval(scratch, 0, 10000);
    expect(call(kernel, g, g.pid, Op::setitimer, {0, scratch, 0}) == 0);
    mock::now = 5020 * ms;
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 88) == 1);
    // The new setting holds although the old value could not be copied out.
    itimerval(scratch, 0, 2000000);
    expect(call(kernel, g, g.pid, Op::setitimer, {0, scratch, 8}) == -14);
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 80) == 2);
    expect(call(kernel, g, g.pid, Op::getitimer, {0, 8}) == -14);
    retire(kernel, g);
}

void posix_timers(Kernel &kernel)
{
    const auto g = create(kernel);
    mock::now = 10000 * ms;
    action(kernel, g, 40);
    // Ids count up per process; a NULL sigevent means SIGALRM carrying the id.
    expect(call(kernel, g, g.pid, Op::timer_create, {1, 0, id_out}) == 0 && get(id_out, 4) == 0);
    expect(call(kernel, g, g.pid, Op::timer_create, {1, 0, id_out}) == 1 && get(id_out, 4) == 1);
    expect(call(kernel, g, g.pid, Op::timer_create, {2, 0, id_out}) == -22);
    expect(call(kernel, g, g.pid, Op::timer_create, {1, 8, id_out}) == -14);
    // A refused sigevent still consumes its id, as in Linux.
    sigevent(0x55, 40, 3); expect(call(kernel, g, g.pid, Op::timer_create, {1, event, id_out}) == -22);
    sigevent(0x55, 0, 0); expect(call(kernel, g, g.pid, Op::timer_create, {1, event, id_out}) == -22);
    sigevent(0x55, 65, 0); expect(call(kernel, g, g.pid, Op::timer_create, {1, event, id_out}) == -22);
    sigevent(0x55, 40, 4, 999); expect(call(kernel, g, g.pid, Op::timer_create, {1, event, id_out}) == -22);
    sigevent(0x55, 40, 0); expect(call(kernel, g, g.pid, Op::timer_create, {1, event, id_out}) == 6);
    expect(call(kernel, g, g.pid, Op::timer_create, {1, event, 8}) == -14);
    // A relative interval timer: the record carries id, value and overrun.
    itimerspec(spec, 10 * ms, 20 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {6, 0, spec, 0}) == 0);
    service(kernel, 10019 * ms); expect(!pending(kernel, g, 40));
    service(kernel, 10020 * ms); expect(pending(kernel, g, 40));
    service(kernel, 10055 * ms);
    native::PrivateProcessQueue::Delivery queued;
    expect(shared(kernel, g).peek({}, queued) &&
           queued.info == (sched::PendingSignalInfo{40, -2, 0, 0, 0, 6, 3, 0x55}));
    // The x86_64 SA_SIGINFO frame sees SI_TIMER, si_tid, si_overrun and si_value.
    auto *p = kernel.scheduler().find(g.pid); auto &leader = thread(kernel, g.pid, g.pid);
    reset_frame(leader); expect(native::private_signal_deliver(kernel, *p, leader));
    expect(leader.native_frame.pc == handler && leader.native_frame.registers[9] == 40);
    const auto info = leader.native_frame.registers[10];
    expect(get(info, 4) == 40 && get(info + 8, 4) == 0xfffffffe && get(info + 16, 4) == 6 &&
           get(info + 20, 4) == 3 && get(info + 24) == 0x55);
    leader.native_signal_mask = {};
    // timer_getoverrun reports the delivered overrun; the timer rearmed past the dequeue.
    mock::now = 10056 * ms;
    expect(call(kernel, g, g.pid, Op::timer_getoverrun, {6}) == 3);
    expect(call(kernel, g, g.pid, Op::timer_gettime, {6, old_spec}) == 0 &&
           spec_interval(old_spec) == 10 * ms && spec_value(old_spec) == 4 * ms);
    // Reprogramming drops the queued record and returns the forwarded old setting.
    service(kernel, 10060 * ms); expect(pending(kernel, g, 40));
    itimerspec(spec, 0, 50 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {6, 0, spec, old_spec}) == 0 && !pending(kernel, g, 40));
    expect(spec_interval(old_spec) == 10 * ms && spec_value(old_spec) == 10 * ms);
    expect(call(kernel, g, g.pid, Op::timer_getoverrun, {6}) == 0);
    // An absolute expiry in the past fires at the next step.
    itimerspec(spec, 0, 9000 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {6, 1, spec, 0}) == 0);
    service(kernel, 10061 * ms); expect(pending(kernel, g, 40) && take(kernel, g).timer == 6);
    // timer_delete removes the queued record and the id.
    itimerspec(spec, 0, 1 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {6, 0, spec, 0}) == 0);
    service(kernel, 10062 * ms); expect(pending(kernel, g, 40));
    expect(call(kernel, g, g.pid, Op::timer_delete, {6}) == 0 && !pending(kernel, g, 40));
    expect(call(kernel, g, g.pid, Op::timer_gettime, {6, old_spec}) == -22);
    expect(call(kernel, g, g.pid, Op::timer_getoverrun, {6}) == -22 && call(kernel, g, g.pid, Op::timer_delete, {6}) == -22);
    expect(call(kernel, g, g.pid, Op::timer_settime, {0, 0, 0, 0}) == -22);
    expect(call(kernel, g, g.pid, Op::timer_settime, {0, 0, 8, 0}) == -14);
    itimerspec(spec, 0, 1 * ms); put(spec + 24, 1000000000);
    expect(call(kernel, g, g.pid, Op::timer_settime, {0, 0, spec, 0}) == -22);
    expect(call(kernel, g, g.pid, Op::timer_settime, {-1ULL, 0, spec, 0}) == -22);
    // Two default timers share SIGALRM: the second record waits for the first's dequeue.
    mock::now = 11000 * ms; itimerspec(spec, 0, 5 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {0, 0, spec, 0}) == 0);
    expect(call(kernel, g, g.pid, Op::timer_settime, {1, 0, spec, 0}) == 0);
    service(kernel, 11005 * ms);
    expect(take(kernel, g) == (sched::PendingSignalInfo{14, -2, 0, 0, 0, 0, 0, 0}) && !pending(kernel, g, 14));
    service(kernel, 11006 * ms);
    expect(take(kernel, g) == (sched::PendingSignalInfo{14, -2, 0, 0, 0, 1, 0, 1}));
    // SIGEV_NONE never signals; timer_gettime forwards its interval.
    sigevent(0, 0, 1); expect(call(kernel, g, g.pid, Op::timer_create, {1, event, id_out}) == 8);
    mock::now = 12000 * ms; itimerspec(spec, 10 * ms, 10 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {8, 0, spec, 0}) == 0);
    service(kernel, 12020 * ms); service(kernel, 12035 * ms);
    native::PrivateProcessQueue::Delivery none;
    expect(!shared(kernel, g).peek({}, none));
    expect(call(kernel, g, g.pid, Op::timer_gettime, {8, old_spec}) == 0 && spec_value(old_spec) == 5 * ms);
    // SIGEV_THREAD_ID queues on its target thread only.
    action(kernel, g, 41);
    sigevent(0x77, 41, 4, static_cast<u32>(g.peer));
    expect(call(kernel, g, g.pid, Op::timer_create, {1, event, id_out}) == 9);
    itimerspec(spec, 0, 1 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {9, 0, spec, 0}) == 0);
    service(kernel, 12036 * ms);
    expect(native::private_signal_queue(thread(kernel, g.pid, g.peer))->timer_queued(41, 9) && !pending(kernel, g, 41));
    // A blocked target with an unqualified wait leaves the expiry owed.
    auto &peer = thread(kernel, g.pid, g.peer);
    native::PrivateSignalQueue::Delivery directed;
    CHECK(native::private_signal_queue(peer)->peek({}, directed) && native::private_signal_queue(peer)->consume(directed));
    expect(call(kernel, g, g.pid, Op::timer_settime, {9, 0, spec, 0}) == 0);
    peer.state = sched::ProcessState::blocked;
    const auto retried = native::private_timer_counters().retried;
    service(kernel, 12037 * ms);
    expect(!native::private_signal_queue(peer)->timer_queued(41, 9) && native::private_timer_counters().retried == retried + 1);
    peer.state = sched::ProcessState::runnable;
    service(kernel, 12038 * ms); expect(native::private_signal_queue(peer)->timer_queued(41, 9));
    // An ignored interval timer parks; once unignored its record reports what it skipped.
    action(kernel, g, 42, 1);
    sigevent(0, 42, 0); expect(call(kernel, g, g.pid, Op::timer_create, {1, event, id_out}) == 10);
    mock::now = 13000 * ms; itimerspec(spec, 10 * ms, 10 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {10, 0, spec, 0}) == 0);
    service(kernel, 13010 * ms); service(kernel, 13035 * ms); expect(!pending(kernel, g, 42));
    action(kernel, g, 42);
    service(kernel, 13036 * ms);
    native::PrivateProcessQueue::Delivery parked;
    expect(shared(kernel, g).peek({}, parked) && parked.info.timer == 10 && parked.info.overrun == 2);
    // Eight timers per process; the ninth fails with EAGAIN.
    for (u32 id = 11; id < 14; ++id)
        expect(call(kernel, g, g.pid, Op::timer_create, {1, 0, id_out}) == id);
    expect(call(kernel, g, g.pid, Op::timer_create, {1, 0, id_out}) == -11);
    retire(kernel, g);
}

void realtime_timers(Kernel &kernel)
{
    const auto g = create(kernel);
    mock::realtime_fail = false; mock::epoch = {1000, 0}; mock::now = 20000 * ms;
    action(kernel, g, 43);
    sigevent(0, 43, 0); expect(call(kernel, g, g.pid, Op::timer_create, {0, event, id_out}) == 0);
    // TIMER_ABSTIME on CLOCK_REALTIME follows the wall clock, including a jump.
    itimerspec(spec, 0, 1000500 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {0, 1, spec, 0}) == 0);
    expect(call(kernel, g, g.pid, Op::timer_gettime, {0, old_spec}) == 0 && spec_value(old_spec) == 500 * ms);
    service(kernel, 20001 * ms); expect(!pending(kernel, g, 43));
    mock::epoch = {1000, 600000000};
    service(kernel, 20002 * ms); expect(pending(kernel, g, 43) && take(kernel, g).timer == 0);
    // A relative CLOCK_REALTIME timer runs on the monotonic clock.
    itimerspec(spec, 0, 100 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {0, 0, spec, 0}) == 0);
    mock::epoch = {5000, 0};
    service(kernel, 20003 * ms); expect(!pending(kernel, g, 43));
    service(kernel, 20102 * ms); expect(pending(kernel, g, 43));
    // Without a wall clock a realtime-based timer waits; arming one reports the failure.
    take(kernel, g);
    itimerspec(spec, 0, 6000 * 1000 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {0, 1, spec, 0}) == 0);
    mock::realtime_fail = true;
    service(kernel, 20200 * ms); expect(!pending(kernel, g, 43));
    expect(call(kernel, g, g.pid, Op::timer_settime, {0, 1, spec, 0}) < 0);
    retire(kernel, g);
}

void lifecycle(Kernel &kernel)
{
    const auto g = create(kernel);
    mock::now = 30000 * ms;
    action(kernel, g, 44);
    itimerval(scratch, 0, 5000000);
    expect(call(kernel, g, g.pid, Op::setitimer, {0, scratch, 0}) == 0);
    sigevent(0, 44, 0); expect(call(kernel, g, g.pid, Op::timer_create, {1, event, id_out}) == 0);
    itimerspec(spec, 0, 1 * ms);
    expect(call(kernel, g, g.pid, Op::timer_settime, {0, 0, spec, 0}) == 0);
    service(kernel, 30001 * ms); expect(pending(kernel, g, 44));
    // exec deletes POSIX timers and their records; ITIMER_REAL survives.
    auto &process = *kernel.scheduler().find(g.pid); auto &leader = process.threads()[0];
    native::private_signal_exec(kernel, process, native::private_signal_queue(leader), *native::private_signal_altstack(leader));
    expect(native::runtime_status().ok() && !pending(kernel, g, 44));
    expect(call(kernel, g, g.pid, Op::timer_gettime, {0, old_spec}) == -22);
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 80) == 4);
    // The id sequence continues after exec.
    expect(call(kernel, g, g.pid, Op::timer_create, {1, 0, id_out}) == 1);
    // Process retirement clears every timer.
    native::private_signal_retire(g.pid);
    expect(call(kernel, g, g.pid, Op::getitimer, {0, scratch + 64}) == 0 && get(scratch + 80) == 0 && get(scratch + 88) == 0);
    expect(call(kernel, g, g.pid, Op::timer_gettime, {1, old_spec}) == -22);
    // Native processes do not take the Linux timer calls.
    process.resources().personality = sched::UserPersonality::native;
    i64 result = 0;
    expect(!native::private_timer_call(kernel, process, leader, result));
    process.resources().personality = sched::UserPersonality::linux;
    retire(kernel, g);
}
} // namespace

int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    itimer_real(kernel); posix_timers(kernel); realtime_timers(kernel); lifecycle(kernel);
    const auto counters = native::private_timer_counters();
    expect(counters.armed && counters.expired && counters.signalled && counters.dequeued && counters.retried);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    std::printf("LINUX_TIMERS_PASS checks=%zu hosted=1 native=0\n", checks);
}
