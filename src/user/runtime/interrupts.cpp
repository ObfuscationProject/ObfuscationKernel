// The interrupt report: how the kernel loop idled, what the platform lines and
// device event sources did, how driver waits completed, timer interrupt latency
// and the interval timer counters (OK_IRQ_PLATFORM, OK_IRQ_EVENTS,
// OK_IRQ_COMPLETIONS, OK_TIMER_LATENCY, OK_TIMERS), with secondary CPUs
// what each CPU did (OK_SMP_CPUS, OK_SMP_IRQS), socket waits (OK_NET_WAITS),
// where kernel sections are compiled in the OK_SMP_KERNEL lines and, with the
// smp_hotplug_test knob, the CPU hotplug cycles (OK_SMP_HOTPLUG).
#include "private.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/smp/bringup.hpp"

namespace ok::user::native
{
// How the kernel loop idled, what the platform lines delivered, how the device
// event sources were served (driven:posted:served:polled) and what woke the
// waits that completed ready or timed out; once the interrupt service runs.
namespace
{
void report_interrupt_lines()
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
    if (timers.armed)
    {
        emit_line("OK_TIMERS scope=hardware armed="); emit_number(timers.armed);
        emit(" expired="); emit_number(timers.expired);
        emit(" signalled="); emit_number(timers.signalled);
        emit(" rearmed="); emit_number(timers.dequeued);
        emit(" retried="); emit_number(timers.retried);
        emit("\n");
    }
#endif
    // With secondary CPUs started, what each CPU did: user entries, local
    // timer ticks, IPIs received and sent, and shootdowns it waited for. The
    // CPUs bring-up started, a hotplug-parked one included (online= counts
    // them), so the per-CPU counters stay comparable across reports.
    const auto started = smp::started_mask();
    if (smp::started_cpus() < 2) return;
    emit_line("OK_SMP_CPUS scope=hardware online="); emit_number(smp::started_cpus());
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
    {
        if ((started & (u32{1} << cpu)) == 0) continue;
        const auto &record = smp::cpu_record(cpu);
        emit(" cpu"); emit_number(cpu); emit("=");
        emit_number(record.user_entries.load(std::memory_order_relaxed));
        emit(":"); emit_number(record.local_ticks.load(std::memory_order_relaxed));
        emit(":"); emit_number(record.ipis_received.load(std::memory_order_relaxed));
        emit(":"); emit_number(record.ipis_sent.load(std::memory_order_relaxed));
        emit(":"); emit_number(record.shootdowns.load(std::memory_order_relaxed));
    }
    // Where device interrupts were served: per CPU, top halves run there,
    // lines whose bottom halves it drained and interrupt waits it entered;
    // then each bound line as line@target CPU.
    emit("\nOK_SMP_IRQS scope=hardware");
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
    {
        if ((started & (u32{1} << cpu)) == 0) continue;
        const auto share = interrupt::platform_irq_cpu_statistics(cpu);
        emit(" cpu"); emit_number(cpu); emit("="); emit_number(share.delivered);
        emit(":"); emit_number(share.serviced); emit(":"); emit_number(share.waits);
    }
    emit(" routes=");
    first = true;
    for (usize line = 0; line < interrupt::max_irq_lines; ++line)
    {
        const auto id = static_cast<interrupt::IrqLine>(line);
        if (!interrupt::platform_irq_bound(id)) continue;
        if (!first) emit(",");
        first = false;
        emit_number(line); emit("@"); emit_number(interrupt::platform_irq_target(id));
    }
    emit("\n");
}
// The first lock violation each CPU reported since the runtime armed its
// checks, and each CPU's count at that point: violations before it (the
// boot test point's deliberate one) are not the runtime's.
struct alignas(64) FirstViolation
{
    std::atomic<bool> recorded{};
    smp::LockViolation violation{};
};
constinit std::array<FirstViolation, smp::max_cpus> first_violations{};
constinit std::array<u64, smp::max_cpus> violation_baseline{};

// Runs on the violating CPU inside the lock checks: it records, never emits.
void record_lock_violation(const smp::LockViolation &violation) noexcept
{
    if (violation.cpu >= smp::max_cpus) return;
    auto &first = first_violations[violation.cpu];
    if (first.recorded.load(std::memory_order_relaxed)) return;
    first.violation = violation;
    first.recorded.store(true, std::memory_order_release);
}

[[maybe_unused]] u64 runtime_lock_violations(smp::CpuId cpu)
{
    const auto count = smp::lock_violation_count(cpu);
    return count > violation_baseline[cpu] ? count - violation_baseline[cpu] : 0;
}

// Kernel sections (OK_SMP_KERNEL, OK_SMP_KERNEL_FAMILIES,
// OK_SMP_KERNEL_DECLINES): what ran without the kernel lock, how its results
// came back, how the kernel lock was contended and what the debug checks
// counted; per CPU sections:concurrent:witnessed:bkl_acquired:bkl_contended.
// On every profile that compiles sections in, with one CPU too, once the
// runtime armed its checks. Any section still pinned is committed first.
void report_kernel_sections()
{
#if defined(OK_SMP_SECTIONS)
    // Only once the runtime armed its checks: the violation baseline is set.
    if (!smp::exclusive_armed.load(std::memory_order_acquire)) return;
    if (smp::kernel_lock.owned_by_current_cpu()) smp::drain_all();
    auto sum = [](std::atomic<uptr> smp::SectionCpu::*counter) {
        u64 total = 0;
        for (const auto &cpu : smp::section_cpus) total += (cpu.*counter).load(std::memory_order_relaxed);
        return total;
    };
    u64 parallel = 0, acquired = 0, contended = 0, violations = 0;
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
    {
        const auto max = smp::section_cpus[cpu].max_parallel.load(std::memory_order_relaxed);
        parallel = max > parallel ? max : parallel;
        acquired += smp::bkl_cpus[cpu].acquired.load(std::memory_order_relaxed);
        contended += smp::bkl_cpus[cpu].contended.load(std::memory_order_relaxed);
        violations += runtime_lock_violations(cpu);
    }
    auto field = [](std::string_view name, u64 value) { emit(" "); emit(name); emit("="); emit_number(value); };
    emit_line("OK_SMP_KERNEL scope=hardware debug="); emit_number(smp::lock_debug_enabled ? 1 : 0);
    field("online", smp::started_cpus());
    field("sections", sum(&smp::SectionCpu::sections));
    field("completed", sum(&smp::SectionCpu::completed));
    field("declined", sum(&smp::SectionCpu::declined));
    field("handoffs", sum(&smp::SectionCpu::handoffs));
    field("concurrent", sum(&smp::SectionCpu::concurrent));
    field("witnessed", sum(&smp::SectionCpu::witnessed));
    field("max_parallel", parallel);
    field("spun", sum(&smp::SectionCpu::spun));
    field("drains", sum(&smp::SectionCpu::drains));
    field("drain_waits", sum(&smp::SectionCpu::drain_waits));
    field("bkl_acquired", acquired);
    field("bkl_contended", contended);
    field("lock_violations", violations);
    field("exclusive_asserts", sum(&smp::SectionCpu::exclusive_asserts));
    field("drain_asserts", sum(&smp::SectionCpu::drain_asserts));
    field("depth_asserts", sum(&smp::SectionCpu::depth_asserts));
    field("section_asserts", sum(&smp::SectionCpu::section_asserts));
    field("cow_refused", sum(&smp::SectionCpu::cow_refused));
    field("purity_faults", sum(&smp::SectionCpu::purity_faults));
    field("shadow", sum(&smp::SectionCpu::shadow_checked));
    emit("/"); emit_number(sum(&smp::SectionCpu::shadow_mismatched));
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
    {
        if ((smp::started_mask() & (u32{1} << cpu)) == 0) continue;
        const auto &section = smp::section_cpus[cpu];
        const auto &lock = smp::bkl_cpus[cpu];
        emit(" cpu"); emit_number(cpu); emit("="); emit_number(section.sections.load(std::memory_order_relaxed));
        emit(":"); emit_number(section.concurrent.load(std::memory_order_relaxed));
        emit(":"); emit_number(section.witnessed.load(std::memory_order_relaxed));
        emit(":"); emit_number(lock.acquired.load(std::memory_order_relaxed));
        emit(":"); emit_number(lock.contended.load(std::memory_order_relaxed));
    }
    emit("\nOK_SMP_KERNEL_FAMILIES scope=hardware");
    for (usize family = 0; family < smp::section_family_count; ++family)
    {
        u64 total = 0;
        for (const auto &cpu : smp::section_cpus) total += cpu.families[family].load(std::memory_order_relaxed);
        field(smp::section_family_names[family], total);
    }
    emit("\nOK_SMP_KERNEL_DECLINES scope=hardware");
    for (usize reason = 0; reason < smp::section_decline_count; ++reason)
    {
        u64 total = 0;
        for (const auto &cpu : smp::section_cpus) total += cpu.declines[reason].load(std::memory_order_relaxed);
        field(smp::section_decline_names[reason], total);
    }
    emit("\n");
    // The first failure per CPU, to locate a non-zero count.
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
    {
        if (first_violations[cpu].recorded.load(std::memory_order_acquire))
        {
            const auto &violation = first_violations[cpu].violation;
            emit("OK_SMP_KERNEL_VIOLATION scope=hardware cpu="); emit_number(cpu);
            emit(" kind="); emit_number(static_cast<u32>(violation.kind));
            emit(" held="); emit_number(static_cast<u32>(violation.held));
            emit(" wanted="); emit_number(static_cast<u32>(violation.wanted));
            emit("\n");
        }
        if (const char *site = smp::section_cpus[cpu].first_site.load(std::memory_order_relaxed))
        {
            emit("OK_SMP_KERNEL_ASSERT scope=hardware cpu="); emit_number(cpu);
            emit(" site="); emit(site); emit("\n");
        }
    }
#endif
}
// The smp_hotplug_test knob: stop/restart cycles of the cycled CPU that
// completed and that failed, with secondary CPUs started.
void report_hotplug()
{
#if defined(OK_SMP_HOTPLUG_TEST)
    if (smp::started_cpus() < 2) return;
    const auto hotplug = smp::hotplug_statistics();
    emit_line("OK_SMP_HOTPLUG scope=hardware cycles="); emit_number(hotplug.cycles);
    emit(" failures="); emit_number(hotplug.failures);
    emit(" cpu="); emit_number(hotplug.cpu);
    emit("\n");
#endif
}
} // namespace
void report_interrupts()
{
    report_interrupt_lines();
    report_network_waits();
    report_kernel_sections();
    report_hotplug();
}
inline namespace runtime_detail
{
void arm_kernel_checks()
{
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu) violation_baseline[cpu] = smp::lock_violation_count(cpu);
    smp::lock_violation_handler = record_lock_violation;
    smp::arm_exclusive();
}
} // namespace runtime_detail
inline namespace runtime_detail
{
// How socket waits were woken (OK_NET_WAITS): waiters enlisted in their
// socket's wait queue, readiness evaluations, and waiters that found their
// queue full; once any socket wait was queued.
constinit const net::TcpWaitStats *tcp_wait_stats{};
void report_network_waits()
{
    const auto tcp = tcp_wait_stats ? *tcp_wait_stats : net::TcpWaitStats{};
    const auto udp = udp_wait_statistics();
    if (!tcp.queued && !tcp.unqueued && !udp.queued && !udp.unqueued) return;
    emit_line("OK_NET_WAITS scope=hardware tcp_queued="); emit_number(tcp.queued);
    emit(" tcp_evaluated="); emit_number(tcp.evaluated);
    emit(" tcp_unqueued="); emit_number(tcp.unqueued);
    emit(" udp_queued="); emit_number(udp.queued);
    emit(" udp_evaluated="); emit_number(udp.evaluated);
    emit(" udp_unqueued="); emit_number(udp.unqueued);
    emit("\n");
}
} // namespace runtime_detail
} // namespace ok::user::native
