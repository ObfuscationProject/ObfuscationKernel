#include "ok/interrupt/irq_chip.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/sched/wait_queue.hpp"
#include "ok/time/clocksource.hpp"
#include "ok/time/interval_timer.hpp"
#include "ok/abi/linux_timer.hpp"
#include "ok/user/native_signal_info.hpp"
#include "interrupt/console_ring.hpp"

#include <cstdio>
#include <cstdlib>

using namespace ok;

#define CHECK(condition)                                                                                   \
    do                                                                                                     \
    {                                                                                                      \
        if (!static_cast<bool>(condition))                                                                 \
        {                                                                                                  \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                            \
            std::abort();                                                                                  \
        }                                                                                                  \
    } while (false)

struct Serviced
{
    int calls{0};
    interrupt::IrqLine last{0};
    interrupt::ModelIrqChip *chip{nullptr};
    bool masked_during{false};
};
static void service(void *context, interrupt::IrqLine line)
{
    auto &seen = *static_cast<Serviced *>(context);
    ++seen.calls;
    seen.last = line;
    seen.masked_during = seen.chip->masked(line);
}

static void check_irq_router()
{
    interrupt::ModelIrqChip chip;
    interrupt::IrqRouter<interrupt::ModelIrqChip> router(chip);
    Serviced seen{.chip = &chip};
    CHECK(router.bind(3, interrupt::IrqTrigger::level, service, &seen).code() == StatusCode::not_initialized);
    CHECK(!router.bound(3));
    CHECK(chip.initialize().ok());
    CHECK(router.bind(3, interrupt::IrqTrigger::level, nullptr, &seen).code() == StatusCode::invalid_argument);
    CHECK(router.bind(3, interrupt::IrqTrigger::level, service, &seen).ok());
    CHECK(router.bind(3, interrupt::IrqTrigger::level, service, &seen).code() == StatusCode::already_exists);
    CHECK(router.bind(64, interrupt::IrqTrigger::edge, service, &seen).code() == StatusCode::invalid_argument);
    CHECK(!chip.masked(3) && !router.pending());
    // Top half: acknowledged, masked, pending and completed; no handler runs yet.
    router.top_half(3);
    CHECK(chip.acknowledged(3) == 1 && chip.completed(3) == 1 && chip.masked(3));
    CHECK(router.pending() && router.delivered(3) == 1 && seen.calls == 0);
    // A second top half before the drain coalesces into one service.
    router.top_half(3);
    CHECK(router.delivered(3) == 2 && chip.completed(3) == 2);
    // Bottom half: one service with the line still masked, then the line is unmasked.
    CHECK(router.drain() == 1);
    CHECK(seen.calls == 1 && seen.last == 3 && seen.masked_during && !chip.masked(3));
    CHECK(!router.pending() && router.serviced(3) == 1 && router.drain() == 0);
    // An unbound line is counted spurious, masked and completed, never serviced.
    router.top_half(9);
    CHECK(router.spurious() == 1 && chip.masked(9) && chip.completed(9) == 1 && !router.pending());
    router.top_half(300);
    CHECK(router.spurious() == 2);
    // A shared line (PCI INTx): a second handler with the same trigger joins it,
    // another trigger is refused, and one bottom half runs both handlers.
    Serviced other{.chip = &chip};
    CHECK(router.bind(3, interrupt::IrqTrigger::edge, service, &other).code() == StatusCode::invalid_argument);
    CHECK(router.bind(3, interrupt::IrqTrigger::level, service, &other).ok());
    router.top_half(3);
    CHECK(router.drain() == 1 && seen.calls == 2 && other.calls == 1 && !chip.masked(3));
    // Removing one handler keeps the line live; removing the last masks it.
    CHECK(router.unbind(3, service, &seen).ok() && router.bound(3) && !chip.masked(3));
    router.top_half(3);
    CHECK(router.drain() == 1 && seen.calls == 2 && other.calls == 2);
    CHECK(router.unbind(3, service, &seen).code() == StatusCode::not_found);
    CHECK(router.unbind(3, service, &other).ok() && chip.masked(3) && !router.bound(3));
    // A line holds at most max_line_handlers handlers.
    Serviced many[interrupt::IrqRouter<interrupt::ModelIrqChip>::max_line_handlers + 1]{};
    for (auto &entry : many)
        entry.chip = &chip;
    for (usize i = 0; i < interrupt::IrqRouter<interrupt::ModelIrqChip>::max_line_handlers; ++i)
        CHECK(router.bind(5, interrupt::IrqTrigger::level, service, &many[i]).ok());
    CHECK(router.bind(5, interrupt::IrqTrigger::level, service,
                      &many[interrupt::IrqRouter<interrupt::ModelIrqChip>::max_line_handlers]).code() ==
          StatusCode::overflow);
    CHECK(router.controller_errors() == 0);
}

static void check_wait_queue()
{
    sched::WaitQueue<2> queue;
    sched::WaitRegistration a, b, c;
    auto ta = queue.prepare(a, 1, 10);
    auto tb = queue.prepare(b, 1, 11);
    CHECK(ta && tb && queue.waiting() == 2);
    CHECK(queue.prepare(c, 1, 12).status().code() == StatusCode::overflow);
    CHECK(!c.active());
    // Condition became ready before sleeping: cancel instead of sleeping.
    CHECK(queue.cancel(tb.value()) && b.completion() == sched::WaitCompletion::cancelled);
    CHECK(!queue.cancel(tb.value()));
    CHECK(queue.wake_one(sched::WakeSource::interrupt));
    CHECK(a.completion() == sched::WaitCompletion::ready && !a.active());
    CHECK(queue.waiting() == 0 && !queue.wake_one(sched::WakeSource::thread));
    CHECK(queue.wakes(sched::WakeSource::interrupt) == 1);
    CHECK(queue.prepare(a, 1, 10) && queue.prepare(b, 1, 11));
    CHECK(queue.wake_all(sched::WakeSource::timer, sched::WaitCompletion::timeout) == 2);
    CHECK(a.completion() == sched::WaitCompletion::timeout && b.completion() == sched::WaitCompletion::timeout);
}

static void check_wake_source()
{
    sched::WaitRegistration registration;
    auto ticket = registration.arm(1, 10);
    CHECK(ticket && registration.source() == sched::WakeSource::thread);
    CHECK(registration.complete(ticket.value(), sched::WaitCompletion::ready, sched::WakeSource::interrupt));
    CHECK(registration.source() == sched::WakeSource::interrupt);
    // A timeout is a timer wake whatever the caller names; arm() resets it.
    ticket = registration.arm(1, 10);
    CHECK(ticket && registration.source() == sched::WakeSource::thread);
    CHECK(registration.complete(ticket.value(), sched::WaitCompletion::timeout, sched::WakeSource::poll));
    CHECK(registration.source() == sched::WakeSource::timer);
    // A refused completion records nothing.
    CHECK(!registration.complete(ticket.value(), sched::WaitCompletion::ready, sched::WakeSource::interrupt));
    CHECK(registration.source() == sched::WakeSource::timer);
}

// The hosted build has no controller, so start_platform_irqs() never runs and
// the event sources are exercised directly.
static void check_event_sources()
{
    using interrupt::EventSource;
    // Undriven: served on every pass, posts dropped.
    interrupt::post_event(EventSource::network);
    CHECK(!interrupt::events_posted());
    CHECK(interrupt::take_event(EventSource::network) && interrupt::take_event(EventSource::network));
    CHECK(interrupt::event_wake_source(EventSource::network) == sched::WakeSource::poll);
    auto stats = interrupt::event_statistics(EventSource::network);
    CHECK(!stats.driven && stats.posted == 0 && stats.served == 0 && stats.polled == 2);
    // Driven: served once per post.
    interrupt::drive_events(EventSource::network);
    CHECK(interrupt::event_driven(EventSource::network) && !interrupt::take_event(EventSource::network));
    interrupt::post_event(EventSource::network);
    interrupt::post_event(EventSource::network);
    CHECK(interrupt::events_posted());
    CHECK(interrupt::take_event(EventSource::network) && !interrupt::events_posted());
    CHECK(!interrupt::take_event(EventSource::network));
    CHECK(interrupt::event_wake_source(EventSource::network) == sched::WakeSource::interrupt);
    stats = interrupt::event_statistics(EventSource::network);
    CHECK(stats.driven && stats.posted == 2 && stats.served == 1 && stats.polled == 2);
    // Sources are independent.
    CHECK(!interrupt::event_driven(EventSource::input) && interrupt::take_event(EventSource::input));
}

struct ModelUart
{
    int fifo[600]{};
    usize head{0}, tail{0}, reads{0};
    void receive(int value) { fifo[head++] = value; }
    int read()
    {
        ++reads;
        return tail == head ? -1 : fifo[tail++];
    }
};

static void check_console_ring()
{
    using interrupt::EventSource;
    ModelUart uart;
    const auto read = [&] { return uart.read(); };
    interrupt::ConsoleRing<4> ring;
    // Before the console is driven the ring reads the UART itself.
    CHECK(!interrupt::event_driven(EventSource::console));
    uart.receive('a');
    CHECK(ring.take(read) == 'a' && ring.take(read) == -1);
    interrupt::drive_events(EventSource::console);
    // Driven: no UART access outside the handler.
    uart.receive('b');
    const usize reads = uart.reads;
    CHECK(!interrupt::take_event(EventSource::console) && ring.take(read) == -1 && uart.reads == reads);
    ring.fill(read);
    CHECK(interrupt::take_event(EventSource::console) && ring.take(read) == 'b');
    CHECK(!interrupt::take_event(EventSource::console) && ring.take(read) == -1);
    // A burst larger than the ring: the handler leaves the rest in the FIFO
    // and take() refills from it once the ring drains, posting until empty.
    for (int value = 0; value < 10; ++value)
        uart.receive('0' + value);
    ring.fill(read);
    for (int value = 0; value < 10; ++value)
        CHECK(interrupt::take_event(EventSource::console) && ring.take(read) == '0' + value);
    CHECK(!interrupt::take_event(EventSource::console));
}

static void check_calibrated_clock()
{
    time::ModelClockSource source(24'000'000); // 24 MHz, like the QEMU virt generic timer
    time::CalibratedClock<time::ModelClockSource> clock(source);
    CHECK(clock.monotonic_ns().status().code() == StatusCode::not_initialized);
    source.advance(1234);
    CHECK(clock.initialize().ok());
    CHECK(clock.monotonic_ns() && clock.monotonic_ns().value() == 0);
    source.advance(24'000'000);
    CHECK(clock.monotonic_ns().value() == time::nanoseconds_per_second);
    source.advance(12'000'000 + 3);
    CHECK(clock.monotonic_ns().value() == 1'500'000'125);
    auto deadline = clock.deadline_ticks(250'000'000);
    CHECK(deadline && deadline.value() == source.counter() + 6'000'000);
    time::ModelClockSource zero(0);
    time::CalibratedClock<time::ModelClockSource> broken(zero);
    CHECK(broken.initialize().code() == StatusCode::invalid_argument);
    // Idle progression: the counter advances without any tick handler running.
    time::ModelClockSource mips(100'000'000);
    time::CalibratedClock<time::ModelClockSource> mips_clock(mips);
    CHECK(mips_clock.initialize().ok());
    mips.advance(50);
    CHECK(mips_clock.monotonic_ns().value() == 500);
}

static void check_interval_timer()
{
    using State = time::IntervalTimer::State;
    using Setting = time::TimerSetting;
    constexpr u64 ms = 1'000'000;
    time::IntervalTimer timer;
    timer.reset(false);
    CHECK(timer.get(0) == Setting{});
    // One shot: an expiry the step has not served yet still reports time left.
    timer.set(1000 * ms, {50 * ms, 0}, false);
    CHECK(timer.get(1010 * ms) == (Setting{40 * ms, 0}) && timer.itimer(1010 * ms) == (Setting{40 * ms, 0}));
    CHECK(!timer.expire(1049 * ms));
    CHECK(timer.get(1060 * ms).value == 1 && timer.itimer(1060 * ms).value == 1000);
    CHECK(timer.expire(1060 * ms) && timer.state() == State::disarmed && timer.preview(1060 * ms) == 0);
    CHECK(!timer.expire(2000 * ms) && timer.get(1070 * ms) == Setting{});
    // An interval timer queues one signal; held 3.5 intervals, it reports three
    // overruns and rearms on its original grid only after the dequeue.
    timer.set(0, {10 * ms, 10 * ms}, false);
    CHECK(timer.expire(10 * ms) && timer.state() == State::signalled && timer.preview(10 * ms) == 0);
    CHECK(!timer.expire(20 * ms) && timer.preview(45 * ms) == 3);
    CHECK(timer.itimer(45 * ms) == (Setting{0, 10 * ms}));
    timer.dequeued();
    CHECK(timer.state() == State::armed && timer.overrun() == 3 && timer.expiry() == 50 * ms);
    CHECK(timer.get(45 * ms) == (Setting{5 * ms, 10 * ms}));
    CHECK(timer.expire(50 * ms) && timer.preview(50 * ms) == 0);
    timer.dequeued();
    CHECK(timer.overrun() == 0 && timer.expiry() == 60 * ms);
    // timer_gettime moves a signalled timer past now and counts what it skipped.
    CHECK(timer.expire(60 * ms) && timer.get(85 * ms) == (Setting{5 * ms, 10 * ms}) && timer.expiry() == 90 * ms);
    CHECK(timer.preview(85 * ms) == 2);
    timer.dequeued();
    CHECK(timer.overrun() == 2 && timer.expiry() == 90 * ms);
    // SIGEV_NONE never expires; observing it forwards an interval timer.
    time::IntervalTimer quiet;
    quiet.reset(true);
    quiet.set(0, {10 * ms, 0}, false);
    CHECK(!quiet.expire(20 * ms) && quiet.get(20 * ms) == Setting{});
    quiet.set(0, {10 * ms, 10 * ms}, false);
    CHECK(quiet.get(35 * ms) == (Setting{5 * ms, 10 * ms}) && quiet.overrun() == 0);
    // Absolute and saturating settings; a zero value disarms and drops the interval.
    timer.set(100, {time::timer_horizon + 5, 7}, true);
    CHECK(timer.expiry() == time::timer_horizon && timer.interval() == 7);
    timer.set(time::timer_horizon - 1, {10, 0}, false);
    CHECK(timer.expiry() == time::timer_horizon);
    timer.set(0, {0, 10 * ms}, false);
    CHECK(timer.state() == State::disarmed && timer.interval() == 0);
    // A dropped ITIMER_REAL signal leaves the timer inactive with its interval.
    timer.set(0, {10, 10}, false);
    CHECK(timer.expire(10));
    timer.abandon();
    CHECK(timer.itimer(100) == (Setting{0, 10}) && !timer.expire(100));
    // Overrun counts clamp to INT_MAX; the rearm saturates at the horizon.
    timer.set(0, {1, 1}, false);
    CHECK(timer.expire(1) && timer.preview(time::timer_horizon) == time::timer_overrun_limit);
    timer.dequeued();
    CHECK(timer.overrun() == time::timer_overrun_limit && timer.expiry() == time::timer_horizon);
}

static void check_timer_codec()
{
    namespace lu = abi::linux_user;
    using X = lu::TimerCodec<lu::Profile::x86_64>;
    // itimerval: interval first, four longs of seconds and microseconds.
    std::array<u8, X::itimerval_bytes> v{};
    v[0] = 1; v[8] = 0x20; v[9] = 0xa1; v[10] = 0x07; v[25] = 0x10;
    auto decoded = X::decode_itimerval(v);
    CHECK(decoded && decoded.value() == (lu::TimerValue{1'500'000'000, 4'096'000}));
    v[8] = 0x40; v[9] = 0x42; v[10] = 0x0f;
    CHECK(!X::decode_itimerval(v));
    v = {}; v[16] = 0xff; for (usize i = 17; i < 24; ++i) v[i] = 0xff;
    CHECK(!X::decode_itimerval(v));
    X::encode_itimerval(v, {1'500'000'999, 3});
    CHECK(v[0] == 1 && v[8] == 0x20 && v[9] == 0xa1 && v[10] == 0x07 && v[16] == 0 && v[24] == 0);
    // Big-endian 32-bit longs.
    using P = lu::TimerCodec<lu::Profile::ppc>;
    std::array<u8, P::itimerval_bytes> b{};
    P::encode_itimerval(b, {0, 2'000'001'000});
    CHECK(b[11] == 2 && b[15] == 1 && b[3] == 0 && b[7] == 0);
    // Legacy and time64 itimerspec on i386; time64 ignores the high nsec half.
    using I = lu::TimerCodec<lu::Profile::i386>;
    std::array<u8, 32> spec{};
    spec[8] = 5; spec[12] = 0xff; spec[13] = 0xc9; spec[14] = 0x9a; spec[15] = 0x3b;
    auto legacy = I::decode_itimerspec({spec.data(), 16}, lu::TimeFormat::legacy_timespec);
    CHECK(legacy && legacy.value() == (lu::TimerValue{0, 5'999'999'999}));
    spec[12] = 0x00; spec[13] = 0xca;
    CHECK(!I::decode_itimerspec({spec.data(), 16}, lu::TimeFormat::legacy_timespec));
    spec = {}; spec[16] = 2; spec[24] = 7; spec[28] = 0xff; spec[31] = 0xff;
    auto wide = I::decode_itimerspec(spec, lu::TimeFormat::kernel_timespec);
    CHECK(wide && wide.value() == (lu::TimerValue{0, 2'000'000'007}));
    // A legacy record keeps the low 32 bits of the seconds, like put_old_timespec32.
    CHECK(I::encode_itimerspec({spec.data(), 16}, lu::TimeFormat::legacy_timespec, {0, 4'294'967'301'000'000'000ULL}));
    CHECK(spec[8] == 5 && spec[9] == 0 && spec[12] == 0);
    // sigevent: the value word in target order, then signo, notify and tid.
    std::array<u8, X::sigevent_bytes> event{};
    event[0] = 0x88; event[7] = 0x11; event[8] = 40; event[12] = 4; event[16] = 0x39; event[17] = 0x30;
    const auto x = X::decode_sigevent(event);
    CHECK(x.value == 0x1100000000000088ULL && x.signal == 40 && x.notify == 4 && x.thread == 12345);
    using M = lu::TimerCodec<lu::Profile::mips64>;
    std::array<u8, M::sigevent_bytes> big{};
    big[3] = 7; big[11] = 50; big[15] = 0;
    const auto m = M::decode_sigevent(big);
    CHECK(m.value == (u64{7} << 32) && m.signal == 50 && m.notify == 0);
}

static void check_timer_records()
{
    using Queue = sched::PendingSignals<64>;
    Queue queue;
    CHECK(queue.reset({1, 1, 1}));
    CHECK(queue.enqueue_timer(14, 3, 0, 99) == Queue::Enqueue::queued);
    CHECK(queue.timer_queued(14, 3) && !queue.timer_queued(14, 4) && !queue.timer_queued(15, 3));
    // A legacy signal number holds one record: a second timer's record coalesced.
    CHECK(queue.enqueue_timer(14, 4, 0, 0) == Queue::Enqueue::coalesced);
    CHECK(queue.refresh_timer(14, 3, 5) && !queue.refresh_timer(14, 4, 5) && !queue.refresh_timer(14, 3, -1));
    Queue::Delivery delivery;
    CHECK(queue.peek({}, delivery));
    CHECK(delivery.info == (sched::PendingSignalInfo{14, -2, 0, 0, 0, 3, 5, 99}));
    CHECK(queue.enqueue_timer(40, 1, 0, 0) == Queue::Enqueue::queued);
    CHECK(queue.enqueue_timer(40, 2, 0, 0) == Queue::Enqueue::queued);
    queue.discard_timer(40, 1);
    CHECK(!queue.timer_queued(40, 1) && queue.timer_queued(40, 2));
    CHECK(queue.enqueue({41, -6, 7, 8}) == Queue::Enqueue::queued);
    queue.discard_timers();
    CHECK(!queue.timer_queued(14, 3) && !queue.timer_queued(40, 2));
    CHECK(queue.peek({}, delivery) && delivery.info.signal == 41 && queue.consume(delivery) && !queue.peek({}, delivery));
    // Generic senders cannot forge timer records or timer payloads.
    CHECK(queue.enqueue({40, -2, 0, 0}) == Queue::Enqueue::invalid);
    CHECK(queue.enqueue({40, -6, 1, 2, 0, 3}) == Queue::Enqueue::invalid);
    CHECK(queue.enqueue_timer(40, -1, 0, 0) == Queue::Enqueue::invalid);
    // MIPS numbers SI_TIMER -3; -2 is SI_ASYNCIO there.
    sched::PendingSignals<128> mips;
    CHECK(mips.reset({1, 1, 1}) && mips.enqueue_timer(40, 0, 1, 0) == sched::PendingSignals<128>::Enqueue::queued);
    sched::PendingSignals<128>::Delivery mips_delivery;
    CHECK(mips.peek({}, mips_delivery) && mips_delivery.info.code == -3);
    CHECK(!(sched::PendingSignalInfo{40, -2, 0, 0, 0, 1}).valid(128));
    // SI_TIMER siginfo: si_tid, si_overrun and the value word after the header.
    using X = user::native::SignalInfoCodec<abi::linux_user::Profile::x86_64>;
    std::array<std::byte, 128> bytes{};
    const sched::PendingSignalInfo timer{40, -2, 0, 0, 0, 5, 3, 0x1122334455667788ULL};
    CHECK(X::encode(timer, bytes) && X::frame_valid(timer));
    CHECK(bytes[0] == std::byte{40} && bytes[8] == std::byte{0xfe} && bytes[11] == std::byte{0xff});
    CHECK(bytes[16] == std::byte{5} && bytes[20] == std::byte{3} && bytes[24] == std::byte{0x88} && bytes[31] == std::byte{0x11});
    using I = user::native::SignalInfoCodec<abi::linux_user::Profile::i386>;
    CHECK(I::encode(timer, bytes) && bytes[12] == std::byte{5} && bytes[16] == std::byte{3} &&
          bytes[20] == std::byte{0x88} && bytes[23] == std::byte{0x55} && bytes[24] == std::byte{0});
    using B = user::native::SignalInfoCodec<abi::linux_user::Profile::mips64>;
    const sched::PendingSignalInfo big{40, -3, 0, 0, 0, 5, 3, 0x0000000700000000ULL};
    CHECK(B::encode(big, bytes) && bytes[3] == std::byte{40} && bytes[7] == std::byte{0xfd});
    CHECK(bytes[19] == std::byte{5} && bytes[23] == std::byte{3} && bytes[27] == std::byte{7} && bytes[31] == std::byte{0});
    // ITIMER_REAL sends SIGALRM as SI_KERNEL; other kernel signals keep their gate.
    CHECK(X::frame_valid(sched::PendingSignalInfo{14, 128, 0, 0}) && !X::frame_valid(sched::PendingSignalInfo{10, 128, 0, 0}));
}

int main()
{
    check_irq_router();
    check_wait_queue();
    check_wake_source();
    check_event_sources();
    check_console_ring();
    check_calibrated_clock();
    check_interval_timer();
    check_timer_codec();
    check_timer_records();
    std::puts("OK_HOST irq_wait_time model=pass");
    return 0;
}
