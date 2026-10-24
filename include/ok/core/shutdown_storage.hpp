#pragma once

#include "ok/core/shutdown.hpp"

namespace ok::fs { class VirtualFileSystem; }
namespace ok::user { class NativeShutdownServices; }

namespace ok
{
// Internal service-boundary attestation, never UAPI or a user-supplied flag.
// Only the trusted runtime adapter may mint it, after the outer boundary,
// admission closure, producer/native retirement and resource drain succeeded.
// The type does not implement those stages or extend their lifetime.
class ShutdownStorageBarrier final
{
  public:
    ShutdownStorageBarrier(const ShutdownStorageBarrier &) = delete;
    ShutdownStorageBarrier &operator=(const ShutdownStorageBarrier &) = delete;
    ShutdownStorageBarrier(ShutdownStorageBarrier &&) = delete;
    ShutdownStorageBarrier &operator=(ShutdownStorageBarrier &&) = delete;
    [[nodiscard]] const ShutdownTicket &ticket() const { return ticket_; }
  private:
    friend class user::NativeShutdownServices;
    explicit ShutdownStorageBarrier(ShutdownTicket ticket) : ticket_(ticket) {}
    const ShutdownTicket ticket_;
};
}

// Optional OS-owned callbacks. Kernel references these as weak hooks. Preflight
// is read-only and grants no producer barrier or clean-storage certificate.
// Seal consumes the original OS owners once; its failure is terminal.
extern "C" ok::Status ok_shutdown_storage_preflight(ok::fs::VirtualFileSystem &, ok::SystemPowerAction);
extern "C" ok::Status ok_shutdown_storage_seal(ok::fs::VirtualFileSystem &, const ok::ShutdownStorageBarrier &);
