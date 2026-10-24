#pragma once

#include "ok/fs/ext4_reclaim_recovery_binding.hpp"
#include "ok/fs/ext4_namespace_recovery_binding.hpp"
#include "ok/fs/ext4_reclaim_composed_history.hpp"

namespace ok::fs
{
template <usize> class Ext4MountedMetadata;
enum class Ext4StartupFamily : u8 { ordinary, namespace_active, reclaim_active, empty_orphan, rejected, legacy_active, typed_zero_active, typed_allocated_active, regular_move_active };
enum class Ext4StartupPhase : u8 { unused, classifying, namespace_checkpointing, reclaim_checkpointing, draining, admitting, complete, blocked, failed, regular_move_checkpointing };
struct Ext4StartupRecoveryReport
{
    Ext4StartupPhase phase{Ext4StartupPhase::unused};
    Ext4StartupFamily initial_family{Ext4StartupFamily::rejected};
    Status status{};
    // Legacy terminal observation precedes ordinary Node construction. Typed
    // common completion records the genuine published root after renewal.
    block::SessionReport original{}, observed{};
    u32 initial_head{}, final_head{}, initial_sequence{}, final_sequence{};
    usize chain_members{}, reclaimed{}, released_blocks{}, verified_publications{};
    usize prospective_members{}, namespace_checkpoints{}, regular_move_checkpoints{};
    bool namespace_result_validated{}, reclaim_result_validated{}, reclaim_dry_route_validated{};
    bool regular_move_result_validated{};
    bool strict_ordinary_admission{}, root_unpublished{true};
};

// Permanent private off-stack storage, disjoint from the live reclaim workspace.
class Ext4MountedStartupWorkspace final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4MountedMetadata;
  public:
    Ext4MountedStartupWorkspace() = default;
    Ext4MountedStartupWorkspace(const Ext4MountedStartupWorkspace &) = delete;
    Ext4MountedStartupWorkspace &operator=(const Ext4MountedStartupWorkspace &) = delete;
    Ext4MountedStartupWorkspace(Ext4MountedStartupWorkspace &&) = delete;
    Ext4MountedStartupWorkspace &operator=(Ext4MountedStartupWorkspace &&) = delete;
  private:
    Status claim(const void *mount, const void *owner)
    {
        smp::ScopedSpinLock guard(lock_);
        if (!mount || !owner || claimant_) return Status::busy("startup workspace already permanently claimed");
        claimant_ = mount; owner_ = owner; return {};
    }
    const void *claimant_{}, *owner_{};
    smp::SpinLock lock_{};
    Ext4OrphanChainCaptureStorage original_{}, sealed_original_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch preparation_{};
    Ext4OrphanReclaimCaptureStorage observed_{};
    Ext4OrphanReclaimColdScratch verification_{};
    Ext4RecoveryBinding terminal_binding_{};
    Ext4ReclaimRecoveryStorage active_reclaim_{};
    Ext4NamespaceRecoveryBinding active_namespace_{};
    Ext4NamespaceResultHistory namespace_history_{};
    Ext4OrphanChainCaptureStorage namespace_result_{};
    Ext4NamespaceSnapshotSeal namespace_result_seal_{};
    Ext4OrphanChainCaptureStorage reclaim_result_{};
    Ext4NamespaceSnapshotSeal reclaim_result_seal_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch reclaim_preparation_{};
    Ext4RecoveryBinding reclaim_ordinary_{};
    Ext4ReclaimComposedStorage reclaim_composed_{};
    Ext4OrphanDirectoryQualifiedReclaimScratch namespace_preparation_{};
    Ext4OrphanBlock locator_super_{}, locator_descriptors_{}, locator_table_{}, locator_head_{}, locator_descriptor_{};
    Ext4OrphanBlock legacy_head_seal_{}, legacy_descriptor_seal_{};
};
} // namespace ok::fs
