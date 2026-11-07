#pragma once

#include "ok/interrupt/irq_chip.hpp"
#include "ok/sched/wait_registration.hpp"
#include "ok/time/latency_histogram.hpp"

namespace ok::interrupt
{

// Platform interrupt service over the profile's PlatformIrqChip. Top halves
// arrive from the architecture trap paths of the CPU a line is routed to,
// with interrupts disabled. Bottom halves run under the kernel lock on any CPU
// that drains: the boot CPU's kernel loop, a secondary CPU's runtime step and
// a driver waiting inside a syscall. The boot CPU's loop consumes the event
// sources and idles in idle_until_interrupt() when nothing is runnable. Every
// call is made with interrupts disabled.
//
// Routing: on a controller that can target CPUs (GICv2 on aarch64, the PLIC
// on rv64, the I/O APIC on x86_64) a line bound while N CPUs are online goes
// to the (line mod N)-th online CPU in logical order; others deliver to CPU 0
// (the 8259 pairs, including x86_64's fallback when the MADT describes no I/O
// APIC, and the uniprocessor profiles). A top half that has to wake a device
// waiter on another CPU with an IPI moves its line to that CPU, unless
// retarget_platform_irq() pinned the line.
struct IrqStatistics
{
    usize waits{0};
    usize ticks{0};
    usize delivered{0};
    usize serviced{0};
    usize spurious{0};
    usize controller_errors{0};
};

// Initializes the controller once and lets the platform bind its own devices.
Status start_platform_irqs();
[[nodiscard]] bool platform_irqs_started();
// True when this profile has a controller driver and can idle on interrupts.
[[nodiscard]] bool platform_irqs_supported();
Status bind_platform_irq(IrqLine line, IrqTrigger trigger, IrqHandler handler, void *context);
Status unbind_platform_irq(IrqLine line, IrqHandler handler, void *context);
// Runs pending bottom halves; the caller holds the kernel lock. Inside a
// kernel section it runs nothing (and bind_platform_irq refuses).
usize drain_platform_irqs();
// From a user-mode TrapKind::interrupt on boards whose trap path does not claim
// the line itself (Malta): claims the controller's pending line and runs its
// top half. A no-op elsewhere.
void claim_platform_irq();
[[nodiscard]] bool platform_irq_pending();
// Halts this CPU, interrupts masked, until it took a device line, a tick (its
// local timer on a secondary CPU) or an IPI; a top half another CPU runs ends
// the wait with an IPI. Returns at once when a line is already pending, and
// false without waiting when the profile cannot idle on interrupts or this
// is a secondary CPU of a profile whose device lines reach CPU 0 only
// (x86_64 on the 8259 pair) or inside a kernel section, whose driver polls.
// A caller holding the kernel lock keeps holding it.
bool wait_for_interrupt();
// The boot CPU's kernel loop idle (kernel lock released): as
// wait_for_interrupt(), but a top half on another CPU does not end it. That
// CPU drains its line itself, and the bottom halves' event posts reach CPU 0
// as IPIs.
bool idle_until_interrupt();
[[nodiscard]] IrqStatistics platform_irq_statistics();
// One CPU's share: `delivered` top halves it ran, `serviced` lines it drained,
// `waits` it entered, `ticks` (CPU 0 only).
[[nodiscard]] IrqStatistics platform_irq_cpu_statistics(smp::CpuId cpu);
// Interrupts delivered to `line` so far (top halves run).
[[nodiscard]] usize platform_irq_delivered(IrqLine line);
[[nodiscard]] bool platform_irq_bound(IrqLine line);
// The CPU a bound line is delivered to.
[[nodiscard]] smp::CpuId platform_irq_target(IrqLine line);
// Moves a bound line to `cpu` (an online CPU), on controllers that route, and
// pins it there: device waits no longer move it.
Status retarget_platform_irq(IrqLine line, smp::CpuId cpu);
// As `cpu` goes offline: spreads its lines over the other online CPUs.
// Hotplug clears its online bit first, so no route() names it afterwards.
// Called on `cpu` itself with interrupts masked, it then serves what its
// controller interface already accepted (an x86 vector in the local APIC's
// IRR, a source pending at its PLIC context, a GIC interrupt signalled to
// it), so the top half runs and completes there and no line is left blocked
// behind an x86 Remote IRR or an unfinished claim. Returns the lines moved.
usize retarget_platform_irqs_away(smp::CpuId cpu);
// Runs the top halves of interrupts pending at this CPU's controller
// interface, and takes its IPIs, without waiting. For a CPU spinning with
// interrupts masked, so that lines routed to it are not stalled.
void serve_local_interrupts();
// Takes the kernel lock; while another CPU holds it, serves this CPU's
// interrupts (a holder may wait for a line routed here).
void lock_kernel_serving_interrupts();

// Device event sources the kernel loop serves. A driver whose line is bound
// calls drive_events() once; from then on its bottom half posts the source and
// the loop serves the source only after a post, never by polling the device.
// A source that is not driven (no bound line, or before start_platform_irqs())
// is polled on every pass. A consumer that stops before the source is empty
// (a budget, a one-byte step) posts it again, which also keeps the loop from
// idling while an undriven source has work left.
enum class EventSource : u8
{
    console,
    input,
    network,
};
inline constexpr usize event_source_count = 3;
struct EventStatistics
{
    bool driven{false};
    usize posted{0};
    usize served{0}; // Passes that served a post.
    usize polled{0}; // Passes that polled the source before it was driven.
};
// The CPU whose kernel loop serves the event sources.
inline constexpr smp::CpuId event_consumer = 0;
void drive_events(EventSource source);
// A post from another CPU that finds the source idle sends the consumer an
// IPI, so its idle wait ends.
void post_event(EventSource source);
// True when the loop should serve `source` on this pass; consumes a post.
[[nodiscard]] bool take_event(EventSource source);
// Drops a post a consumer cannot serve (its link retired), without counting a
// served or polled pass.
void discard_event(EventSource source);
[[nodiscard]] bool event_driven(EventSource source);
// A source has a post the loop has not taken yet.
[[nodiscard]] bool events_posted();
// The wake source a waiter made ready by `source`'s events is credited to.
[[nodiscard]] sched::WakeSource event_wake_source(EventSource source);
[[nodiscard]] EventStatistics event_statistics(EventSource source);

// Timer interrupt latency: how long after its programmed expiry the kernel
// observed a timer interrupt, for the tick that ends the idle wait and for the
// tick that preempts user code. Architecture code reports `late` ticks of its
// `frequency` Hz timer counter.
enum class TimerWake : u8
{
    idle,
    user,
};
void record_timer_latency(TimerWake wake, u64 late, u64 frequency);
[[nodiscard]] const time::LatencyHistogram &timer_latency(TimerWake wake);

// Driver waits for a device (a block or entropy request, a blocking transmit)
// by how they waited: on the device's bound line, reading the device once and
// then only after its own interrupt (a completion the first read already saw
// counts here too), or by polling, before the service started, without a line,
// with polling selected or inside a kernel section (where wait_for_interrupt()
// refuses, so a wait on a bound line is counted as polled).
enum class DeviceWait : u8
{
    block,
    entropy,
    transmit,
};
inline constexpr usize device_wait_count = 3;
struct DeviceWaitStatistics
{
    usize interrupt{0};
    usize polled{0};
};
void count_device_wait(DeviceWait device, bool interrupt);
[[nodiscard]] DeviceWaitStatistics device_wait_statistics(DeviceWait device);

// Board routing for device drivers: the platform line a device interrupts on.
struct PlatformLine
{
    IrqLine line{};
    IrqTrigger trigger{IrqTrigger::level};
};
// QEMU virt's virtio-mmio transport `slot` (arm32, aarch64, rv64, rv32).
[[nodiscard]] Result<PlatformLine> virtio_mmio_line(usize slot);
// PCI bus 0 device `device`, interrupt pin `pin` (1 = INTA .. 4 = INTD).
// `interrupt_line` is its configuration register 0x3c, which only boards whose
// firmware routes INTx (the PC's SeaBIOS) fill in. On x86_64 routing through
// the I/O APIC the line is the GSI that ISA IRQ reaches (MADT overrides).
[[nodiscard]] Result<PlatformLine> pci_intx_line(u8 device, u8 pin, u8 interrupt_line);

} // namespace ok::interrupt

// Architecture trap entry points, called with interrupts disabled.
extern "C" void ok_interrupt_top_half(ok::interrupt::IrqLine line);
extern "C" void ok_interrupt_tick();
// An IPI reached this CPU: its interrupt wait ends as after a tick.
extern "C" void ok_interrupt_wake();
// Optional platform hook run once by start_platform_irqs() to bind the
// platform's own devices (for example the console UART).
extern "C" ok::Status ok_platform_bind_irqs() __attribute__((weak));
