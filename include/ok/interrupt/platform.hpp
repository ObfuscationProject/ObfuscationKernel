#pragma once

#include "ok/interrupt/irq_chip.hpp"

namespace ok::interrupt
{

// Boot-CPU platform interrupt service over the profile's PlatformIrqChip.
// Top halves arrive from the architecture trap paths with interrupts
// disabled; the kernel loop drains bottom halves and idles in
// wait_for_interrupt() when nothing is runnable. Every call is made with
// interrupts disabled.
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
Status unbind_platform_irq(IrqLine line);
usize drain_platform_irqs();
[[nodiscard]] bool platform_irq_pending();
// Enables interrupts, halts until one has been taken and disables them again.
// Returns false without waiting when the profile cannot idle on interrupts.
bool wait_for_interrupt();
[[nodiscard]] IrqStatistics platform_irq_statistics();

} // namespace ok::interrupt

// Architecture trap entry points, called with interrupts disabled.
extern "C" void ok_interrupt_top_half(ok::interrupt::IrqLine line);
extern "C" void ok_interrupt_tick();
// Optional platform hook run once by start_platform_irqs() to bind the
// platform's own devices (for example the console UART).
extern "C" ok::Status ok_platform_bind_irqs() __attribute__((weak));
