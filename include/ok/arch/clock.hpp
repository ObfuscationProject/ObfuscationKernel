#pragma once

#include "ok/core/types.hpp"
#include "ok/time/epoch.hpp"

namespace ok::arch
{

// A boot-CPU clock for the supported QEMU platform profile. The epoch is the
// first successful initialization, not civil time. Initialization is idempotent.
// Calls must be serialized on the boot CPU; this is not an SMP clock contract.
Status initialize_clock();
[[nodiscard]] Result<u64> monotonic_nanoseconds();
[[nodiscard]] Result<time::MonotonicPoint> monotonic_sample();
[[nodiscard]] Status clock_status();
// Terminal for this boot. Call before known reset/suspend/restore/frequency
// changes; this cannot detect missed whole hardware-counter wraps by itself.
void invalidate_clock();
// Returns zero before successful initialization, otherwise the counter's
// conversion granularity rounded upward to whole nanoseconds (not accuracy).
[[nodiscard]] u64 clock_resolution_ns();

} // namespace ok::arch
