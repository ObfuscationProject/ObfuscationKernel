// Linux personality interval timers: ITIMER_REAL (setitimer, getitimer, alarm)
// and the timer_create family. The runtime step expires them on the boot CPU
// and queues their signals; an interval timer rearms once the step observes
// that its signal left the queue (Linux dequeue-time rearm).
#include "private.hpp"
#include "ok/abi/linux_timer.hpp"
#include "ok/time/interval_timer.hpp"

namespace ok::user::native
{
inline namespace runtime_detail
{
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
namespace
{
using TimerCodec = lu::TimerCodec<private_signal_profile>;
using TimerActions = SignalActionCodec<private_signal_profile>;
using TimerState = time::IntervalTimer::State;
constexpr i32 timer_code = sched::PendingSignalInfo::timer_code(TimerActions::max_signal);
constexpr u32 alarm_signal = 14;
constexpr i32 clock_realtime = 0, clock_monotonic = 1;
constexpr u64 timer_second = 1000000000;
// POSIX timers per process; Linux bounds them only by memory.
constexpr usize posix_timers_per_process = 8;

struct PosixTimer
{
    bool used{};
    i32 id{};
    bool realtime_clock{}; // Created on CLOCK_REALTIME.
    bool realtime{};       // Armed TIMER_ABSTIME on CLOCK_REALTIME: expiry is wall-clock time.
    u32 signal{};
    sched::ThreadId thread{}; // SIGEV_THREAD_ID target; zero is process-directed.
    u64 value{};
    bool owed{};    // Expired, its signal not queued yet (parked while ignored).
    bool stalled{}; // Its target thread is gone; nothing will dequeue the signal.
    time::IntervalTimer timer{};
};
struct ProcessTimers
{
    sched::ProcessId pid{};
    time::IntervalTimer real{};
    bool real_owed{};
    i32 next_id{};
    std::array<PosixTimer, posix_timers_per_process> posix{};
};
constinit std::array<ProcessTimers, sched::max_processes> process_timers{};
constinit PrivateTimerCounters timer_counters{};

ProcessTimers *find_timers(sched::ProcessId pid)
{
    for (auto &entry : process_timers)
        if (pid && entry.pid == pid) return &entry;
    return nullptr;
}
ProcessTimers *acquire_timers(sched::ProcessId pid)
{
    if (auto *entry = find_timers(pid)) return entry;
    for (auto &entry : process_timers)
        if (!entry.pid)
        {
            entry = {};
            entry.pid = pid;
            return &entry;
        }
    return nullptr;
}
PosixTimer *find_posix(ProcessTimers *timers, i32 id)
{
    if (!timers || id < 0) return nullptr;
    for (auto &timer : timers->posix)
        if (timer.used && timer.id == id) return &timer;
    return nullptr;
}
Result<u64> realtime_now()
{
    auto now = arch::realtime();
    if (!now) return now.status();
    const auto value = now.value();
    if (value.seconds < 0) return u64{0};
    const auto seconds = static_cast<u64>(value.seconds);
    return seconds >= time::timer_horizon / timer_second ? time::timer_horizon : seconds * timer_second + value.nanoseconds;
}
Result<u64> base_now(const PosixTimer &timer, u64 monotonic)
{
    return timer.realtime ? realtime_now() : Result<u64>(monotonic);
}
// The queue that holds a POSIX timer's record: its target thread or the process.
template <typename Visit>
bool with_queue(Kernel &kernel, sched::ProcessId pid, const PosixTimer &timer, Visit &&visit)
{
    if (timer.thread)
    {
        auto *thread = kernel.scheduler().find_thread(pid, timer.thread);
        auto *queue = thread && thread->state != sched::ProcessState::exited ? private_signal_queue(*thread) : nullptr;
        return queue && visit(*queue);
    }
    auto *state = private_signal_find(pid);
    return state && visit(state->shared);
}
bool record_queued(Kernel &kernel, sched::ProcessId pid, const PosixTimer &timer)
{
    return with_queue(kernel, pid, timer, [&](auto &queue) { return queue.timer_queued(timer.signal, timer.id); });
}
void discard_record(Kernel &kernel, sched::ProcessId pid, const PosixTimer &timer)
{
    static_cast<void>(with_queue(kernel, pid, timer, [&](auto &queue) {
        queue.discard_timer(timer.signal, timer.id);
        return true;
    }));
}
// A signalled interval timer whose record left its queue was dequeued (or its
// ignored record discarded) at the last preview; it rearms past that time.
void settle_posix(Kernel &kernel, sched::ProcessId pid, PosixTimer &timer)
{
    if (timer.timer.state() != TimerState::signalled || timer.owed || timer.stalled || record_queued(kernel, pid, timer))
        return;
    timer.timer.dequeued();
    ++timer_counters.dequeued;
}
void settle_real(ProcessTimers &timers)
{
    if (timers.real.state() != TimerState::signalled || timers.real_owed) return;
    const auto *state = private_signal_find(timers.pid);
    if (state && state->shared.pending_mask().contains(alarm_signal)) return;
    timers.real.dequeued();
    ++timer_counters.dequeued;
}
void service_real(Kernel &kernel, sched::ProcessControlBlock &process, ProcessTimers &timers, u64 now)
{
    auto &real = timers.real;
    settle_real(timers);
    if (real.expire(now))
    {
        timers.real_owed = true;
        ++timer_counters.expired;
    }
    if (!timers.real_owed)
    {
        if (real.state() == TimerState::signalled) static_cast<void>(real.preview(now));
        return;
    }
    // Linux it_real_fn sends SIGALRM as SI_KERNEL to the process.
    const auto sent = private_timer_signal(kernel, process, 0, {alarm_signal, 128, 0, 0});
    if (sent == PrivateTimerSend::retry) { ++timer_counters.retried; return; }
    timers.real_owed = false;
    if (sent == PrivateTimerSend::sent)
    {
        ++timer_counters.signalled;
        static_cast<void>(real.preview(now));
    }
    else if (real.state() == TimerState::signalled)
        real.abandon(); // A dropped SIGALRM is never dequeued.
}
void service_posix(Kernel &kernel, sched::ProcessControlBlock &process, PosixTimer &timer, u64 now)
{
    settle_posix(kernel, process.pid(), timer);
    if (timer.timer.expire(now))
    {
        timer.owed = true;
        ++timer_counters.expired;
    }
    if (!timer.owed)
    {
        // Keep the queued record's overrun current until it is dequeued.
        if (timer.timer.state() == TimerState::signalled && !timer.stalled)
        {
            const auto overrun = timer.timer.preview(now);
            static_cast<void>(with_queue(kernel, process.pid(), timer, [&](auto &queue) {
                return queue.refresh_timer(timer.signal, timer.id, overrun);
            }));
        }
        return;
    }
    const sched::PendingSignalInfo info{timer.signal, timer_code, 0, 0, 0, timer.id, timer.timer.preview(now), timer.value};
    switch (private_timer_signal(kernel, process, timer.thread, info))
    {
    case PrivateTimerSend::sent:
        timer.owed = false;
        ++timer_counters.signalled;
        break;
    case PrivateTimerSend::ignored:
        // Linux parks an ignored interval timer until its signal is unignored.
        if (timer.timer.state() != TimerState::signalled) timer.owed = false;
        break;
    case PrivateTimerSend::departed:
        timer.owed = false;
        timer.stalled = true;
        break;
    case PrivateTimerSend::retry:
        ++timer_counters.retried;
        break;
    }
}
i64 timer_failure(Status status)
{
    return -static_cast<i64>(syscall::ErrnoMapper::errno_for(status));
}
// timer_create writes one int timer id.
bool copy_timer_id(hw::AddressSpace &space, u64 address, i32 id)
{
    std::array<u8, 4> bytes{};
    for (usize i = 0; i < 4; ++i)
        bytes[i] = static_cast<u8>(static_cast<u32>(id) >> (8 * (TimerCodec::Abi::profile.big_endian ? 3 - i : i)));
    return hw::copy_to_user(space, address, bytes.data(), bytes.size()).ok();
}
// The default sigevent carries the timer id as sival_int, the union's first int.
constexpr u64 default_value(i32 id)
{
    const auto value = static_cast<u64>(static_cast<u32>(id));
    return TimerCodec::Abi::profile.big_endian && TimerCodec::word == 8 ? value << 32 : value;
}

i64 create_timer(Kernel &kernel, sched::ProcessControlBlock &process, const std::array<u64, 6> &a)
{
    auto &space = process.resources().native_space;
    const auto clock = static_cast<i32>(static_cast<u32>(a[0]));
    lu::SignalEvent event{0, static_cast<i32>(alarm_signal), lu::sigev_signal, 0};
    if (a[1])
    {
        std::array<u8, TimerCodec::sigevent_bytes> bytes{};
        if (!hw::copy_from_user(space, bytes.data(), a[1], bytes.size())) return -14;
        event = TimerCodec::decode_sigevent(bytes);
    }
    // clock_gettime serves these two clocks only; CPU-time clocks are absent.
    if (clock != clock_realtime && clock != clock_monotonic) return -22;
    auto *timers = acquire_timers(process.pid());
    if (!timers) return -11;
    PosixTimer *slot = nullptr;
    for (auto &timer : timers->posix)
        if (!timer.used) { slot = &timer; break; }
    if (!slot) return -11;
    // Linux allocates the id first; a refused sigevent still consumes it.
    i32 id = timers->next_id;
    while (find_posix(timers, id)) id = (id + 1) & 0x7fffffff;
    timers->next_id = (id + 1) & 0x7fffffff;
    if (!a[1]) event.value = default_value(id);
    sched::ThreadId target{};
    if (event.notify == lu::sigev_thread_id)
    {
        const auto *thread = event.thread > 0 ? kernel.scheduler().find_thread(process.pid(), static_cast<u32>(event.thread))
                                              : nullptr;
        if (!thread || thread->state == sched::ProcessState::exited) return -22;
        target = thread->tid;
    }
    else if (event.notify != lu::sigev_signal && event.notify != lu::sigev_thread && event.notify != lu::sigev_none)
        return -22;
    if (event.notify != lu::sigev_none && (event.signal <= 0 || static_cast<u32>(event.signal) > TimerActions::max_signal))
        return -22;
    if (!copy_timer_id(space, a[2], id)) return -14;
    *slot = {};
    slot->used = true;
    slot->id = id;
    slot->realtime_clock = clock == clock_realtime;
    slot->signal = event.notify == lu::sigev_none ? 0 : static_cast<u32>(event.signal);
    slot->thread = target;
    slot->value = event.value;
    slot->timer.reset(event.notify == lu::sigev_none);
    return id;
}

i64 set_timer(Kernel &kernel, sched::ProcessControlBlock &process, const std::array<u64, 6> &a,
              lu::TimeFormat format, u64 monotonic)
{
    auto &space = process.resources().native_space;
    if (!a[2]) return -22;
    const auto size = TimerCodec::itimerspec_bytes(format);
    std::array<u8, 32> bytes{};
    if (!hw::copy_from_user(space, bytes.data(), a[2], size)) return -14;
    auto next = TimerCodec::decode_itimerspec({bytes.data(), size}, format);
    if (!next) return -22;
    auto *timer = find_posix(find_timers(process.pid()), static_cast<i32>(static_cast<u32>(a[0])));
    if (!timer) return -22;
    settle_posix(kernel, process.pid(), *timer);
    lu::TimerValue old{};
    if (a[3])
    {
        auto now = base_now(*timer, monotonic);
        if (!now) return timer_failure(now.status());
        const auto setting = timer->timer.get(now.value());
        old = {setting.interval, setting.value};
    }
    const bool absolute = (static_cast<u32>(a[1]) & 1U) != 0;
    // Relative CLOCK_REALTIME timers run on the monotonic base (common_hrtimer_arm).
    const bool realtime = timer->realtime_clock && absolute;
    auto now = realtime ? realtime_now() : Result<u64>(monotonic);
    if (!now) return timer_failure(now.status());
    // A reprogrammed timer drops the signal its earlier setting queued.
    discard_record(kernel, process.pid(), *timer);
    timer->owed = false;
    timer->stalled = false;
    timer->realtime = realtime;
    timer->timer.set(now.value(), {next.value().value, next.value().interval}, absolute);
    if (next.value().value) ++timer_counters.armed;
    if (a[3])
    {
        if (auto status = TimerCodec::encode_itimerspec({bytes.data(), size}, format, old); !status)
            return timer_failure(status);
        if (!hw::copy_to_user(space, a[3], bytes.data(), size)) return -14;
    }
    return 0;
}

i64 get_timer(Kernel &kernel, sched::ProcessControlBlock &process, const std::array<u64, 6> &a,
              lu::TimeFormat format, u64 monotonic)
{
    auto *timer = find_posix(find_timers(process.pid()), static_cast<i32>(static_cast<u32>(a[0])));
    if (!timer) return -22;
    settle_posix(kernel, process.pid(), *timer);
    auto now = base_now(*timer, monotonic);
    if (!now) return timer_failure(now.status());
    const auto setting = timer->timer.get(now.value());
    const auto size = TimerCodec::itimerspec_bytes(format);
    std::array<u8, 32> bytes{};
    if (auto status = TimerCodec::encode_itimerspec({bytes.data(), size}, format, {setting.interval, setting.value}); !status)
        return timer_failure(status);
    return hw::copy_to_user(process.resources().native_space, a[1], bytes.data(), size) ? 0 : -14;
}

i64 set_itimer(sched::ProcessControlBlock &process, const std::array<u64, 6> &a, u64 now)
{
    auto &space = process.resources().native_space;
    lu::TimerValue next{};
    std::array<u8, TimerCodec::itimerval_bytes> bytes{};
    if (a[1])
    {
        if (!hw::copy_from_user(space, bytes.data(), a[1], bytes.size())) return -14;
        auto decoded = TimerCodec::decode_itimerval(bytes);
        if (!decoded) return -22;
        next = decoded.value();
    }
    // ITIMER_VIRTUAL and ITIMER_PROF need CPU-time accounting, which is absent.
    if (static_cast<i32>(static_cast<u32>(a[0])) != 0) return -22;
    auto *timers = acquire_timers(process.pid());
    if (!timers) return -11;
    settle_real(*timers);
    const auto old = timers->real.itimer(now);
    timers->real.set(now, {next.value, next.interval}, false);
    if (next.value) ++timer_counters.armed;
    if (!a[2]) return 0;
    TimerCodec::encode_itimerval(bytes, {old.interval, old.value});
    return hw::copy_to_user(space, a[2], bytes.data(), bytes.size()) ? 0 : -14;
}

i64 get_itimer(sched::ProcessControlBlock &process, const std::array<u64, 6> &a, u64 now)
{
    if (static_cast<i32>(static_cast<u32>(a[0])) != 0) return -22;
    auto *timers = find_timers(process.pid());
    if (timers) settle_real(*timers);
    const auto setting = timers ? timers->real.itimer(now) : time::TimerSetting{};
    std::array<u8, TimerCodec::itimerval_bytes> bytes{};
    TimerCodec::encode_itimerval(bytes, {setting.interval, setting.value});
    return hw::copy_to_user(process.resources().native_space, a[1], bytes.data(), bytes.size()) ? 0 : -14;
}

// alarm_setitimer: rounds the old remainder up to whole seconds.
i64 set_alarm(sched::ProcessControlBlock &process, u64 raw, u64 now)
{
    u64 seconds = static_cast<u32>(raw);
    if (TimerCodec::word == 4 && seconds > 0x7fffffff) seconds = 0x7fffffff;
    auto *timers = acquire_timers(process.pid());
    if (!timers) return -11;
    settle_real(*timers);
    const auto old = timers->real.itimer(now);
    timers->real.set(now, {seconds * timer_second, 0}, false);
    if (seconds) ++timer_counters.armed;
    u64 result = old.value / timer_second;
    const u64 fraction = old.value % timer_second;
    if ((!result && fraction) || fraction >= timer_second / 2) ++result;
    return static_cast<i64>(result);
}
} // namespace

bool private_timer_call(Kernel &kernel, sched::ProcessControlBlock &process,
                        sched::ThreadControlBlock &thread, i64 &result)
{
    if (process.resources().personality != Personality::linux) return false;
    const auto call = hw::syscall(thread.native_frame);
    using Operation = lu::Operation;
    auto is = [&](Operation operation) {
        const auto number = TimerCodec::Abi::syscall_number(operation);
        return number && call.number == *number;
    };
    const bool settime = is(Operation::timer_settime) || is(Operation::timer_settime32) || is(Operation::timer_settime64);
    const bool gettime = is(Operation::timer_gettime) || is(Operation::timer_gettime32) || is(Operation::timer_gettime64);
    if (!settime && !gettime && !is(Operation::getitimer) && !is(Operation::setitimer) && !is(Operation::alarm) &&
        !is(Operation::timer_create) && !is(Operation::timer_getoverrun) && !is(Operation::timer_delete))
        return false;
    std::array<u64, 6> a{};
    for (usize index = 0; index < a.size(); ++index) a[index] = call.arguments[index];
    auto now = arch::monotonic_nanoseconds();
    if (!now) { result = timer_failure(now.status()); return true; }
    const auto format = TimerCodec::word == 8 || is(Operation::timer_settime64) || is(Operation::timer_gettime64)
        ? lu::TimeFormat::kernel_timespec : lu::TimeFormat::legacy_timespec;
    if (is(Operation::getitimer)) result = get_itimer(process, a, now.value());
    else if (is(Operation::setitimer)) result = set_itimer(process, a, now.value());
    else if (is(Operation::alarm)) result = set_alarm(process, a[0], now.value());
    else if (is(Operation::timer_create)) result = create_timer(kernel, process, a);
    else if (settime) result = set_timer(kernel, process, a, format, now.value());
    else if (gettime) result = get_timer(kernel, process, a, format, now.value());
    else
    {
        auto *timer = find_posix(find_timers(process.pid()), static_cast<i32>(static_cast<u32>(a[0])));
        if (!timer) { result = -22; return true; }
        settle_posix(kernel, process.pid(), *timer);
        if (is(Operation::timer_getoverrun)) result = timer->timer.overrun();
        else
        {
            discard_record(kernel, process.pid(), *timer);
            *timer = {};
            result = 0;
        }
    }
    return true;
}

bool private_timer_service(Kernel &kernel, u64 now)
{
    bool pending = false;
    for (auto &timers : process_timers)
    {
        if (!timers.pid) continue;
        auto *process = kernel.scheduler().find(timers.pid);
        if (!process || process->state() == sched::ProcessState::zombie ||
            process->state() == sched::ProcessState::exited)
        {
            timers = {};
            continue;
        }
        service_real(kernel, *process, timers, now);
        pending |= timers.real.state() != TimerState::disarmed || timers.real_owed;
        for (auto &timer : timers.posix)
        {
            if (!timer.used) continue;
            auto base = base_now(timer, now);
            if (!base) continue; // A realtime timer waits for the wall clock.
            service_posix(kernel, *process, timer, base.value());
            pending |= timer.timer.state() != TimerState::disarmed || timer.owed;
        }
        if (!runtime_failure) return pending;
    }
    return pending;
}

void private_timer_exec(sched::ProcessId pid)
{
    if (auto *timers = find_timers(pid))
        for (auto &timer : timers->posix) timer = {};
}

void private_timer_retire(sched::ProcessId pid)
{
    if (auto *timers = find_timers(pid)) *timers = {};
}

PrivateTimerCounters private_timer_counters()
{
    return timer_counters;
}
#endif
} // namespace runtime_detail
} // namespace ok::user::native
