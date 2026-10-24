#pragma once

#include "ok/fs/ext4_orphan_directory_refs.hpp"
#include "ok/fs/ext4_orphan_reclaim_plan.hpp"

namespace ok::fs
{
struct Ext4OrphanDirectoryQualifiedReclaimScratch
{
    Ext4OrphanDirectoryReferenceScratch directory{};
    Ext4OrphanOwnershipScratch reclaim{};
    // Full home images stay in caller storage instead of nested 10 KiB frames.
    Ext4OrphanReclaimPlan candidate{}, transformation{};
};

struct Ext4OrphanDirectoryQualifiedReclaimPlan
{
    Ext4OrphanDirectoryReferenceProof directory{};
    Ext4OrphanReclaimPlan reclaim{};
    bool valid{};
};

// Pure composition over one immutable caller-supplied image set. A valid
// result proves no parsed directory name references any orphan in that set;
// it does not grant device, cache, descriptor-lifetime, journal or freeing
// authority. Scratch and output must not alias the borrowed input images.
class Ext4OrphanDirectoryQualifiedReclaimPlanner final
{
  public:
    [[nodiscard]] static Status plan(
        const Ext4RecoveryInfo &binding,
        const Ext4OrphanDirectoryReferenceSource &source,
        Ext4OrphanDirectoryQualifiedReclaimScratch &scratch,
        u32 expected_head, u32 expected_generation,
        Ext4OrphanDirectoryQualifiedReclaimPlan &output)
    {
        output = {};
        Ext4OrphanDirectoryReferenceProof directory{};
        if (auto status = Ext4OrphanDirectoryReferenceValidator::validate(
                binding, source, scratch.directory, directory); !status)
            return status;
        if (!directory.valid || directory.filesystem_uuid != binding.uuid ||
            directory.observed_head != expected_head)
            return Status::io_error("directory qualified reclaim identity differs");

        auto &reclaim = scratch.candidate;
        if (auto status = Ext4OrphanReclaimPlanner::plan_off_stack(
                binding, source.ownership, scratch.reclaim,
                expected_head, expected_generation, scratch.transformation, reclaim); !status)
            return status;
        if (reclaim.inode != expected_head ||
            reclaim.old_generation != expected_generation || !reclaim.count)
            return Status::fault("directory qualified reclaim plan identity differs");

        output.directory = directory;
        output.reclaim = reclaim;
        output.valid = true;
        return {};
    }
};
} // namespace ok::fs
