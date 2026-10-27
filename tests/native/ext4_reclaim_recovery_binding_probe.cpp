#include "ok/fs/ext4/recovery.hpp"
#include <type_traits>
template class ok::fs::Ext4ReclaimRecoveryBinding<2>;

using HistoryRegistry = ok::block::BlockSessions<2>;
using HistorySession = HistoryRegistry::Session;
using HistoryGuard = ok::fs::Ext4ReclaimRouteBudget<HistorySession>;
using GuardedBinding = ok::fs::Ext4ReclaimRecoveryBinding<2>;
using GuardedReclaimCapture = ok::fs::Ext4OrphanReclaimCapture<2>;
static_assert(ok::block::ReadDispatchGuard<HistoryGuard, HistorySession>);
static_assert(!std::is_copy_constructible_v<HistoryGuard> && !std::is_move_constructible_v<HistoryGuard>);

// Compile-only public surfaces. All large storage belongs to the caller, and
// each independent Guard is borrowed only for its current synchronous call.
extern "C" ok::Status ext4_reclaim_recovery_guard_probe(GuardedBinding &binding,
    HistorySession &session, ok::fs::Ext4ReclaimRecoveryStorage &storage,
    ok::fs::Ext4OrphanReclaimColdScratch &scratch)
{
    {
        HistoryGuard guard;
        if (auto status = guard.reserve_opening(session); !status) return status;
        if (auto status = binding.inspect(session, storage, scratch, guard); !status) return status;
        if (auto status = guard.close(session); !status) return status;
    }
    HistoryGuard guard;
    if (auto status = guard.reserve_opening(session); !status) return status;
    if (auto status = binding.revalidate(guard); !status) return status;
    return guard.close(session);
}

extern "C" ok::Status ext4_reclaim_capture_guard_probe(GuardedReclaimCapture &capture,
    HistorySession &session, const ok::fs::Ext4RecoveryInfo &binding,
    ok::fs::Ext4OrphanReclaimCaptureStorage &storage,
    ok::fs::Ext4OrphanReclaimColdScratch &scratch)
{
    {
        HistoryGuard guard;
        if (auto status = guard.reserve_opening(session); !status) return status;
        if (auto status = capture.capture(session, binding, storage, scratch, guard); !status) return status;
        if (auto status = guard.close(session); !status) return status;
    }
    HistoryGuard guard;
    if (auto status = guard.reserve_opening(session); !status) return status;
    if (auto status = capture.revalidate(guard); !status) return status;
    return guard.close(session);
}
