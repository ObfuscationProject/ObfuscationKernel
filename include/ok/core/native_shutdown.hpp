#pragma once

#include "ok/core/shutdown.hpp"

namespace ok
{
class Kernel;
namespace detail { [[noreturn]] void desktop_loop(bool show_shell); }

// Only the actual outer loop can mint this scoped kernel-internal provenance.
// A service stack or an inactive entry slot alone does not establish it.
class NativeShutdownBoundary final
{
  public:
    NativeShutdownBoundary(const NativeShutdownBoundary &) = delete;
    NativeShutdownBoundary &operator=(const NativeShutdownBoundary &) = delete;
  private:
    friend void detail::desktop_loop(bool);
    NativeShutdownBoundary() = default;
};

// No caller identity comes from an argument. These capture the native runtime's
// canonical running PCB/TCB. They are internal APIs, not a user ABI.
Result<ShutdownTicket> prepare_native_shutdown(Kernel &, SystemPowerAction);
Status commit_native_shutdown(Kernel &, ShutdownTicket);
// Scalar user selectors never expose the internal ticket or authorize a caller.
// Both operations authenticate the canonical original init on every invocation.
Result<u32> prepare_native_power(Kernel &, u32 action);
Status commit_native_power(Kernel &, u32 token);
bool native_shutdown_pending();
Result<SystemPowerAction> advance_native_shutdown(Kernel &, const NativeShutdownBoundary &);
} // namespace ok
