#pragma once

#include "ok/arch/realtime.hpp"

// Hosted tests only: this source is never part of the kernel or exported SDK.
// Legacy mounts must not consult this unavailable-by-default clock.
namespace ok::testing::ext4_clock
{
void reset();
void set(time::RealtimeSample sample);
void fail(Status status = Status::not_initialized("hosted ext4 clock unavailable"));
[[nodiscard]] usize calls();
} // namespace ok::testing::ext4_clock
