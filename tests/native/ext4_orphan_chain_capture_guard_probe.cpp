#include <ok/fs/ext4/recovery.hpp>
#include <type_traits>

using CaptureRegistry = ok::block::BlockSessions<1>;
using CaptureSession = CaptureRegistry::Session;
using CaptureGuard = ok::fs::Ext4ReclaimRouteBudget<CaptureSession>;
using GuardedCapture = ok::fs::Ext4OrphanChainCapture<1>;
static_assert(ok::block::ReadDispatchGuard<CaptureGuard, CaptureSession>);
static_assert(!std::is_copy_constructible_v<CaptureGuard> && !std::is_move_constructible_v<CaptureGuard>);

// The caller owns large capture/scratch objects; a Guard never survives its
// synchronous call or authorizes writer dispatch. This probe performs no I/O.
extern "C" ok::Status ext4_orphan_chain_capture_guard_probe(GuardedCapture &capture,
    CaptureSession &session, ok::fs::Ext4OrphanChainCaptureStorage &storage,
    ok::fs::Ext4OrphanDirectoryQualifiedReclaimScratch &scratch,
    ok::fs::Ext4OrphanDirectoryQualifiedReclaimPlan &plan)
{
    {
        CaptureGuard guard;
        if (auto status = guard.reserve_opening(session); !status) return status;
        if (auto status = capture.capture(session, storage, scratch, guard); !status) return status;
        if (auto status = guard.close(session); !status) return status;
    }
    CaptureGuard guard;
    if (auto status = guard.reserve_opening(session); !status) return status;
    const auto &report = capture.report();
    if (auto status = capture.plan_head(report.head, report.generation, plan, guard); !status) return status;
    return guard.close(session);
}
