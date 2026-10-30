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
Status unbind_platform_irq(IrqLine line, IrqHandler handler, void *context);
usize drain_platform_irqs();
// From a user-mode TrapKind::interrupt on boards whose trap path does not claim
// the line itself (Malta): claims the controller's pending line and runs its
// top half. A no-op elsewhere.
void claim_platform_irq();
[[nodiscard]] bool platform_irq_pending();
// Enables interrupts, halts until one has been taken and disables them again.
// Returns false without waiting when the profile cannot idle on interrupts.
bool wait_for_interrupt();
[[nodiscard]] IrqStatistics platform_irq_statistics();
// Interrupts delivered to `line` so far (top halves run).
[[nodiscard]] usize platform_irq_delivered(IrqLine line);
[[nodiscard]] bool platform_irq_bound(IrqLine line);

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
// firmware routes INTx (the PC's SeaBIOS) fill in.
[[nodiscard]] Result<PlatformLine> pci_intx_line(u8 device, u8 pin, u8 interrupt_line);

} // namespace ok::interrupt

// Architecture trap entry points, called with interrupts disabled.
extern "C" void ok_interrupt_top_half(ok::interrupt::IrqLine line);
extern "C" void ok_interrupt_tick();
// Optional platform hook run once by start_platform_irqs() to bind the
// platform's own devices (for example the console UART).
extern "C" ok::Status ok_platform_bind_irqs() __attribute__((weak));
