#pragma once

#include "ok/smp/lock.hpp"

namespace ok::smp
{

// The big kernel lock: every CPU's kernel loop holds it while it touches
// kernel state (scheduler, runtime tables, VFS, allocators, device bottom
// halves) and releases it only while that CPU runs user code or idles. User
// code of different threads therefore runs concurrently; kernel work is
// serialized. Finer locks rank after it.
inline constinit RankedSpinLock<LockRank::big_kernel> kernel_lock{};

} // namespace ok::smp
