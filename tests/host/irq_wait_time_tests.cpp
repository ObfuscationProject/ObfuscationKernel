#include "ok/interrupt/irq_chip.hpp"
#include "ok/sched/wait_queue.hpp"
#include "ok/time/clocksource.hpp"

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
    CHECK(router.unbind(3).ok() && chip.masked(3) && !router.bound(3));
    CHECK(router.unbind(3).code() == StatusCode::not_found);
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
    check_calibrated_clock();
    std::puts("OK_HOST irq_wait_time model=pass");
    return 0;
}
