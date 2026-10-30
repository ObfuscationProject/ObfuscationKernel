#include "ok/interrupt/irq_chip.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/sched/wait_queue.hpp"
#include "ok/time/clocksource.hpp"
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

int main()
{
    check_irq_router();
    check_wait_queue();
    check_wake_source();
    check_event_sources();
    check_console_ring();
    check_calibrated_clock();
    std::puts("OK_HOST irq_wait_time model=pass");
    return 0;
}
