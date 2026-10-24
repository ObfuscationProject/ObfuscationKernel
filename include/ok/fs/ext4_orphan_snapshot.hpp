#pragma once

#include "ok/fs/ext4_orphan_chain_capture.hpp"

namespace ok::fs
{
// Cold readonly registration policy delegates to the same complete capture
// mechanism used by a live owner's existing session. Only this wrapper owns
// acquisition and closure; it never upgrades the session to writable access.
template <usize DeviceCapacity = 4>
class Ext4OrphanSnapshotAdmission final
{
    using Registry = block::BlockSessions<DeviceCapacity>;
  public:
    Ext4OrphanSnapshotAdmission() = default;
    Ext4OrphanSnapshotAdmission(const Ext4OrphanSnapshotAdmission &) = delete;
    Ext4OrphanSnapshotAdmission &operator=(const Ext4OrphanSnapshotAdmission &) = delete;
    Ext4OrphanSnapshotAdmission(Ext4OrphanSnapshotAdmission &&) = delete;
    Ext4OrphanSnapshotAdmission &operator=(Ext4OrphanSnapshotAdmission &&) = delete;

    [[nodiscard]] Status open(Registry &registry, typename Registry::Registration key,
        block::SessionBounds bounds, Ext4OrphanSnapshotStorage &storage,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch)
    {
        if (attempted_) return Status::already_exists("orphan snapshot is one-shot");
        attempted_ = true; report_.phase = Ext4OrphanSnapshotPhase::opening;
        if (auto status = registry.acquire(key, bounds, block::SessionAccess::readonly, session_); !status)
        {
            report_.phase = Ext4OrphanSnapshotPhase::rejected; report_.status = status;
            return status;
        }
        delegated_ = true;
        return capture_.capture(session_, storage, scratch);
    }
    [[nodiscard]] Status plan_head(u32 head, u32 generation, Ext4OrphanDirectoryQualifiedReclaimPlan &output)
    {
        output = {};
        if (!delegated_) return Status::not_initialized("orphan snapshot is not admitted");
        return capture_.plan_head(head, generation, output);
    }
    [[nodiscard]] Status close()
    {
        if (report().phase != Ext4OrphanSnapshotPhase::admitted)
            return Status::busy("orphan snapshot cannot close in current phase");
        if (auto status = session_.close(); !status) return capture_.reject(status);
        report_ = capture_.report(); report_.valid = false;
        report_.phase = Ext4OrphanSnapshotPhase::closed; delegated_ = false;
        return {};
    }
    [[nodiscard]] const Ext4OrphanSnapshotReport &report() const
    { return delegated_ ? capture_.report() : report_; }
  private:
    typename Registry::Session session_{};
    Ext4OrphanChainCapture<DeviceCapacity> capture_{};
    Ext4OrphanSnapshotReport report_{};
    bool attempted_{}, delegated_{};
};
} // namespace ok::fs
