#pragma once

#include "ok/interrupt/platform.hpp"

namespace ok::interrupt
{

// The platform line of PC ISA IRQ `irq` (i386, x86_64), which the device
// drives with `trigger`: the 8259 line of that number or, once x86_64 routes
// through the I/O APIC, the GSI the MADT moves it to, with the override's
// trigger mode when it names one. Not the tick (0) or the cascade (2).
[[nodiscard]] Result<PlatformLine> isa_irq_line(u8 irq, IrqTrigger trigger);

} // namespace ok::interrupt
