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

static int handled_calls = 0;
static Status count_handler(void *, arch::TrapFrame &)
{
    ++handled_calls;
    return Status::success();
}

static void check_irq_router()
{
    interrupt::ModelIrqChip chip;
    interrupt::InterruptDispatcher dispatcher;
    interrupt::IrqRouter<interrupt::ModelIrqChip> router(chip, dispatcher);
    arch::TrapFrame frame{};
    CHECK(router.bind(3, 40, interrupt::IrqTrigger::level).code() == StatusCode::not_initialized);
    CHECK(chip.initialize().ok());
    CHECK(router.bind(3, 40, interrupt::IrqTrigger::level).ok());
    CHECK(router.bind(3, 41, interrupt::IrqTrigger::level).code() == StatusCode::already_exists);
    CHECK(router.bind(64, 41, interrupt::IrqTrigger::edge).code() == StatusCode::invalid_argument);
    CHECK(!chip.masked(3));
    CHECK(router.on_interrupt(3, frame).code() == StatusCode::not_found); // no handler yet
    CHECK(chip.acknowledged(3) == 1 && chip.completed(3) == 1);
    CHECK(dispatcher.register_callback(40, "count", nullptr, count_handler).ok());
    CHECK(router.on_interrupt(3, frame).ok());
    CHECK(handled_calls == 1 && router.delivered(3) == 2 && dispatcher.handled_count(40) == 1);
    CHECK(router.on_interrupt(9, frame).code() == StatusCode::not_found);
    CHECK(router.spurious() == 1);
    CHECK(router.unbind(3).ok() && chip.masked(3));
    CHECK(router.unbind(3).code() == StatusCode::not_found);
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
