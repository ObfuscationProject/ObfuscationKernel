#pragma once

#include "ok/time/realtime_clock.hpp"

namespace ok::arch
{

// Retained QEMU board, UTC RTC base, boot CPU, serialized calls, no suspend or
// migration. The source's representable epoch domain must not have wrapped.
// Runtime initializes the one-time anchor before exposing checked public clocks.
// Current IRQ/NMI handlers must not call these services; the boot-CPU main loop
// owns the full monotonic-sample-through-read/commit interval. This is not a
// universal IRQ-masked or SMP-safe interface. See docs/qualification/REALTIME_SETTING.md.
[[nodiscard]] Result<time::EpochReading> read_epoch_source();
Status initialize_realtime();
[[nodiscard]] Result<time::EpochTime> realtime();
// Retained physical boot-anchor diagnostic, unaffected by software selection.
[[nodiscard]] time::AnchorInfo realtime_info();
[[nodiscard]] Result<time::RealtimeSample> realtime_sample();
[[nodiscard]] time::RealtimeInfo realtime_clock_info();
// Trusted serialized commit after user validation/authority and one real sample.
// A reading on another CPU between that sample and this commit makes it fail
// continuity; callers that can race readers use set_realtime_now.
Status set_realtime(time::EpochTime value, time::MonotonicPoint point);
// Trusted commit after user validation/authority: samples the monotonic clock
// and commits under the lock every realtime reader holds
// (time::commit_realtime_setting).
Status set_realtime_now(time::EpochTime value);

} // namespace ok::arch
