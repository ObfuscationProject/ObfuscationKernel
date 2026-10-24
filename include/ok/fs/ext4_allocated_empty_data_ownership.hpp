#pragma once

#include "ok/fs/ext4_allocated_empty_data_plan.hpp"

namespace ok::fs
{
struct Ext4AllocatedEmptyDataOwnership
{
    u32 inode{}, generation{}, group{}, physical{};
    u64 old_version{};
    bool complete{};
};
struct Ext4AllocatedEmptyDataOwnershipScratch
{
    Ext4ZeroTruncateOwnershipScratch empty{};
    Ext4ZeroTruncateOwnership inspected{};
    Ext4AllocatedEmptyDataScratch planner{};
    Ext4AllocatedEmptyDataPlan canonical{};
    Ext4AllocatedEmptyDataOwnership proposed{};
    Ext4FirstDataBlock bitmap{}, inode_bitmap{}, inode_table{}, bookend{};
};

// Read-only candidate facts over a retained exclusive view. The global proof
// checks all allocated records, reserved mappings and allocation counts. A free
// candidate therefore has no allocated reference. Exact source regeneration
// and final bookends bind these facts to the complete proposed old homes.
// The caller still owns exclusion, device/session lifetime and dispatch rights.
class Ext4AllocatedEmptyDataOwnershipValidator final
{
    static bool overlaps(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    template <typename A, typename B>
    static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
  public:
    template <jbd2::BlockReader Reader>
    [[nodiscard]] static Status prove(const Ext4RecoveryInfo &binding, Ext4ReadOnlyView<Reader> &view,
        const Ext4AllocatedEmptyDataPlan &plan, Ext4AllocatedEmptyDataOwnershipScratch &scratch,
        Ext4AllocatedEmptyDataOwnership &output)
    {
        using namespace ext4_checked;
        if (overlap(scratch, output) || overlap(scratch, binding) || overlap(scratch, view) ||
            overlap(scratch, plan) || overlap(output, binding) || overlap(output, view) || overlap(output, plan))
            return Status::invalid_argument("allocated empty ownership input/workspace overlap");
        if (view.reader_ && (overlap(scratch, *view.reader_) || overlap(output, *view.reader_)))
            return Status::invalid_argument("allocated empty ownership retained reader/workspace overlap");
        scratch = {}; output = {};
        if (plan.count != 4) return Status::invalid_argument("allocated empty ownership plan shape");
        const Ext4ZeroTruncateUpdate identity{plan.update.inode, plan.update.generation, 0, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove(binding, view, identity,
                scratch.empty, scratch.inspected); !status) return status;
        if (scratch.inspected.count || scratch.inspected.old_size)
            return Status::io_error("allocated empty ownership target is not empty");
        const u32 groups = u32(binding.metadata_count / 4);
        const u32 per_group = le32(scratch.empty.superblock, 40);
        if (!per_group || plan.group >= groups)
            return Status::invalid_argument("allocated empty ownership candidate group");
        const u32 inode_group = (plan.update.inode - 1) / per_group;
        if (inode_group >= groups) return Status::invalid_argument("allocated empty ownership inode group");
        const usize data_at = usize(plan.group) * 32, inode_at = usize(inode_group) * 32;
        const u32 bitmap = le32(scratch.empty.descriptors, data_at);
        const u32 inode_bitmap = le32(scratch.empty.descriptors, inode_at + 4);
        const u32 local = (plan.update.inode - 1) % per_group;
        const u32 table = le32(scratch.empty.descriptors, inode_at + 8) + local / 4;
        if (auto status = view.block(bitmap, scratch.bitmap); !status) return status;
        if (auto status = view.block(inode_bitmap, scratch.inode_bitmap); !status) return status;
        if (auto status = view.block(table, scratch.inode_table); !status) return status;
        for (usize at = 0; at < 256; ++at)
            if (scratch.inode_table[(local % 4) * 256 + at] != scratch.empty.inode[at])
                return view.failure(Status::io_error("allocated empty ownership target changed after global proof"));
        const Ext4AllocatedEmptyDataSource source{plan.group, plan.physical, table,
            scratch.empty.superblock, scratch.empty.descriptors, scratch.bitmap,
            scratch.inode_bitmap, scratch.inode_table};
        if (auto status = Ext4AllocatedEmptyDataPlanner::plan_off_stack(binding, plan.update,
                source, scratch.planner, scratch.canonical); !status) return status;
        const auto &canonical = scratch.canonical;
        if (canonical.count != plan.count || canonical.group != plan.group || canonical.physical != plan.physical ||
            canonical.old_version != plan.old_version || canonical.new_version != plan.new_version ||
            canonical.targets != plan.targets || canonical.before != plan.before || canonical.after != plan.after)
            return Status::io_error("allocated empty ownership plan differs from exclusive source");
        const std::array<u32, 5> numbers{1, 2, bitmap, inode_bitmap, table};
        const std::array<const Ext4FirstDataBlock *, 5> images{&scratch.empty.superblock,
            &scratch.empty.descriptors, &scratch.bitmap, &scratch.inode_bitmap, &scratch.inode_table};
        for (usize index = 0; index < numbers.size(); ++index)
        {
            if (auto status = view.block(numbers[index], scratch.bookend); !status) return status;
            if (scratch.bookend != *images[index])
                return view.failure(Status::io_error("allocated empty ownership final source bookend changed"));
        }
        scratch.proposed = {plan.update.inode, plan.update.generation, plan.group, plan.physical, plan.old_version, true};
        output = scratch.proposed;
        return {};
    }
};
} // namespace ok::fs
