// The interrupt report: how the kernel loop idled, what the platform lines and
// device event sources did, how driver waits completed, timer interrupt latency
// and the interval timer counters (OK_IRQ_PLATFORM, OK_IRQ_EVENTS,
// OK_IRQ_COMPLETIONS, OK_TIMER_LATENCY, OK_TIMERS).
#include "private.hpp"
#include "ok/interrupt/platform.hpp"

namespace ok::user::native
{
// How the kernel loop idled, what the platform lines delivered, how the device
// event sources were served (driven:posted:served:polled) and what woke the
// waits that completed ready or timed out; once the interrupt service runs.
void report_interrupts()
{
    if (!interrupt::platform_irqs_started()) return;
    const auto irqs = interrupt::platform_irq_statistics();
    emit_line("OK_IRQ_PLATFORM scope=hardware waits="); emit_number(irqs.waits);
    emit(" ticks="); emit_number(irqs.ticks);
    emit(" delivered="); emit_number(irqs.delivered);
    emit(" serviced="); emit_number(irqs.serviced);
    emit(" user_interrupts="); emit_number(entry_interrupts);
    emit(" spurious="); emit_number(irqs.spurious);
    emit(" controller_errors="); emit_number(irqs.controller_errors);
    // Bound lines as line:delivered.
    emit(" lines=");
    bool first = true;
    for (usize line = 0; line < interrupt::max_irq_lines; ++line)
    {
        const auto id = static_cast<interrupt::IrqLine>(line);
        if (!interrupt::platform_irq_bound(id)) continue;
        if (!first) emit(",");
        first = false;
        emit_number(line); emit(":"); emit_number(interrupt::platform_irq_delivered(id));
    }
    emit("\nOK_IRQ_EVENTS scope=hardware");
    constexpr std::string_view sources[interrupt::event_source_count]{"console", "input", "network"};
    for (usize source = 0; source < interrupt::event_source_count; ++source)
    {
        const auto events = interrupt::event_statistics(static_cast<interrupt::EventSource>(source));
        emit(" "); emit(sources[source]); emit("="); emit_number(events.driven ? 1 : 0);
        emit(":"); emit_number(events.posted); emit(":"); emit_number(events.served);
        emit(":"); emit_number(events.polled);
    }
    if (runtime_kernel)
    {
        const auto &scheduler = runtime_kernel->scheduler();
        emit(" wakes=thread:"); emit_number(scheduler.wakes(sched::WakeSource::thread));
        emit(",timer:"); emit_number(scheduler.wakes(sched::WakeSource::timer));
        emit(",interrupt:"); emit_number(scheduler.wakes(sched::WakeSource::interrupt));
        emit(",poll:"); emit_number(scheduler.wakes(sched::WakeSource::poll));
    }
    // Driver waits as interrupt:polled.
    emit("\nOK_IRQ_COMPLETIONS scope=hardware");
    constexpr std::string_view waits[interrupt::device_wait_count]{"block", "entropy", "transmit"};
    for (usize device = 0; device < interrupt::device_wait_count; ++device)
    {
        const auto completed = interrupt::device_wait_statistics(static_cast<interrupt::DeviceWait>(device));
        emit(" "); emit(waits[device]); emit("="); emit_number(completed.interrupt);
        emit(":"); emit_number(completed.polled);
    }
    // Timer interrupts after their programmed expiry, as samples:min:p50:p99:max;
    // the percentiles are bucket bounds within an eighth of their value.
    emit("\nOK_TIMER_LATENCY scope=hardware unit=ns");
    constexpr std::string_view wakes[2]{"idle", "user"};
    for (usize wake = 0; wake < 2; ++wake)
    {
        const auto &latency = interrupt::timer_latency(static_cast<interrupt::TimerWake>(wake));
        emit(" "); emit(wakes[wake]); emit("="); emit_number(latency.count());
        emit(":"); emit_number(latency.minimum()); emit(":"); emit_number(latency.percentile(500));
        emit(":"); emit_number(latency.percentile(990)); emit(":"); emit_number(latency.maximum());
    }
    emit("\n");
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
    // Interval timers once any was armed: settings, expiries, signals queued,
    // rearms after a dequeue and steps an expiry stayed owed.
    const auto timers = private_timer_counters();
    if (!timers.armed) return;
    emit_line("OK_TIMERS scope=hardware armed="); emit_number(timers.armed);
    emit(" expired="); emit_number(timers.expired);
    emit(" signalled="); emit_number(timers.signalled);
    emit(" rearmed="); emit_number(timers.dequeued);
    emit(" retried="); emit_number(timers.retried);
    emit("\n");
#endif
}
} // namespace ok::user::native
