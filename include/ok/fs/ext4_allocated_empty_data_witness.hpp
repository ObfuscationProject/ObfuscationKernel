#pragma once

#include "ok/fs/ext4_allocated_empty_data_plan.hpp"

namespace ok::fs
{
struct Ext4AllocatedEmptyDataWitness
{
    Ext4AllocatedEmptyDataUpdate update{};
    std::array<u8, 16> uuid{};
    std::array<u32, 4> targets{};
    u32 group{}, physical{}, sequence{};
    u64 old_version{}, new_version{};
};
struct Ext4AllocatedEmptyDataWitnessScratch
{
    Ext4FirstDataBlock block{}, encoded{};
    Ext4AllocatedEmptyDataPlan reconstructed{}, canonical{};
    Ext4AllocatedEmptyDataScratch planner{};
};

// Image facts only. Exact old-inode bytes and checked inverse allocation
// reconstruct the four old homes; the pure planner must regenerate every byte.
// Neither this descriptor extension nor CRCs grant transport or recovery rights.
class Ext4AllocatedEmptyDataWitnessCodec final
{
    using Block = Ext4FirstDataBlock;
    using Parent = Ext4FirstDataPlanner;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'A', 'E', 'W', '0', '0', '1'};
    static void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static void store32(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
    static bool overlaps(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    template <typename A, typename B>
    static bool overlap(const A &a, const B &b)
    { return overlaps(&a, sizeof(a), &b, sizeof(b)); }
    static bool same_plan(const Ext4AllocatedEmptyDataPlan &a, const Ext4AllocatedEmptyDataPlan &b)
    {
        return a.update.inode == b.update.inode && a.update.generation == b.update.generation &&
            a.update.size == b.update.size && a.update.seconds == b.update.seconds &&
            a.update.nanoseconds == b.update.nanoseconds && a.group == b.group &&
            a.physical == b.physical && a.old_version == b.old_version && a.new_version == b.new_version &&
            a.targets == b.targets && a.before == b.before && a.after == b.after && a.count == b.count;
    }
    static Status regenerate(const Ext4RecoveryInfo &binding, const Block &inode_bitmap,
        Ext4AllocatedEmptyDataWitnessScratch &scratch)
    {
        const auto &p = scratch.reconstructed;
        const Ext4AllocatedEmptyDataSource source{p.group, p.physical, p.targets[3],
            p.before[2], p.before[1], p.before[0], inode_bitmap, p.before[3]};
        if (auto status = Ext4AllocatedEmptyDataPlanner::plan_off_stack(binding, p.update,
                source, scratch.planner, scratch.canonical); !status) return status;
        return same_plan(p, scratch.canonical) ? Status{} :
            Status::io_error("allocated empty witness is not the canonical four-home transform");
    }
    static Status check_tags(const Ext4RecoveryInfo &binding, const Block &descriptor,
        std::span<const Block, 4> after, Ext4AllocatedEmptyDataWitnessScratch &scratch)
    {
        using namespace ext4_checked;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const auto sequence = std::span<const u8>{descriptor}.subspan(8, 4);
        for (usize at = 28; at < 44; ++at)
            if (descriptor[at]) return Status::io_error("allocated empty first tag UUID padding");
        for (usize at = 92; at < begin; ++at)
            if (descriptor[at]) return Status::io_error("allocated empty tag padding");
        for (usize home = 0; home < 4; ++home)
        {
            const usize at = home ? 44 + (home - 1) * 16 : 12;
            const u32 target = le32(descriptor, 192 + home * 4);
            const bool escaped = jbd2::detail::be32(after[home], 0) == jbd2::magic;
            const u32 flags = (home ? jbd2::flag_same_uuid : 0) |
                (home == 3 ? jbd2::flag_last_tag : 0) | (escaped ? jbd2::flag_escape : 0);
            if (!target || target >= binding.layout.filesystem_blocks ||
                jbd2::detail::be32(descriptor, at) != target ||
                jbd2::detail::be32(descriptor, at + 4) != flags ||
                jbd2::detail::be32(descriptor, at + 8) ||
                le32(descriptor, 224 + home * 4) != jbd2::detail::crc32c(~u32{0}, after[home]))
                return Status::io_error("allocated empty whole after-home or tag binding");
            for (usize prior = 0; prior < home; ++prior)
                if (le32(descriptor, 192 + prior * 4) == target)
                    return Status::io_error("allocated empty duplicate home target");
            scratch.block = after[home];
            if (escaped) store32(scratch.block, 0, 0);
            if (jbd2::detail::be32(descriptor, at + 12) !=
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), scratch.block))
                return Status::io_error("allocated empty payload tag checksum");
        }
        return {};
    }
  public:
    static constexpr usize begin = 128, inode_begin = 240, length = 496, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize index = 0; index < magic.size(); ++index)
            if (descriptor[begin + index] != magic[index]) return false;
        return true;
    }
    // Every failure preserves descriptor. Its final CRC is stamped by the
    // journal formatter after the extension is installed in valid tags.
    [[nodiscard]] static Status encode(const Ext4RecoveryInfo &binding,
        const Ext4AllocatedEmptyDataPlan &plan, const Block &inode_bitmap, u32 sequence,
        Ext4AllocatedEmptyDataWitnessScratch &scratch, Block &descriptor)
    {
        using namespace ext4_checked;
        if (overlap(scratch, plan) || overlap(scratch, binding) || overlap(scratch, inode_bitmap) ||
            overlap(scratch, descriptor) || overlap(descriptor, plan) ||
            overlap(descriptor, binding) || overlap(descriptor, inode_bitmap))
            return Status::invalid_argument("allocated empty witness encode workspace overlap");
        if (plan.count != 4 || !sequence || jbd2::detail::be32(descriptor, 0) != jbd2::magic ||
            jbd2::detail::be32(descriptor, 4) != u32(jbd2::RecordType::descriptor) ||
            jbd2::detail::be32(descriptor, 8) != sequence)
            return Status::invalid_argument("allocated empty witness plan or descriptor sequence");
        scratch.reconstructed = plan;
        if (auto status = regenerate(binding, inode_bitmap, scratch); !status) return status;
        const u32 per_group = le32(plan.before[2], 40);
        const usize offset = usize((plan.update.inode - 1) % per_group % 4) * 256;
        scratch.encoded = descriptor;
        for (usize at = begin; at < end; ++at) scratch.encoded[at] = 0;
        for (usize index = 0; index < magic.size(); ++index) scratch.encoded[begin + index] = magic[index];
        store16(scratch.encoded, 136, 1); store16(scratch.encoded, 138, u16(length));
        scratch.encoded[140] = 4;
        for (usize index = 0; index < binding.uuid.size(); ++index) scratch.encoded[144 + index] = binding.uuid[index];
        store32(scratch.encoded, 160, sequence); store32(scratch.encoded, 164, plan.update.inode);
        store32(scratch.encoded, 168, plan.update.generation); store32(scratch.encoded, 172, plan.update.size);
        store32(scratch.encoded, 176, plan.group); store32(scratch.encoded, 180, plan.physical);
        store32(scratch.encoded, 184, plan.update.seconds); store32(scratch.encoded, 188, plan.update.nanoseconds);
        for (usize home = 0; home < 4; ++home)
        {
            store32(scratch.encoded, 192 + home * 4, plan.targets[home]);
            store32(scratch.encoded, 208 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            store32(scratch.encoded, 224 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.after[home]));
        }
        for (usize index = 0; index < 256; ++index)
            scratch.encoded[inode_begin + index] = plan.before[3][offset + index];
        if (auto status = check_tags(binding, scratch.encoded, plan.after, scratch); !status) return status;
        descriptor = scratch.encoded;
        return {};
    }
    // Requires logical unescaped after images and a separately retained,
    // checked inode bitmap. Non-alias failures clear both output fact records.
    [[nodiscard]] static Status decode(const Ext4RecoveryInfo &binding, const Block &descriptor,
        std::span<const Block, 4> after, const Block &inode_bitmap,
        Ext4AllocatedEmptyDataWitnessScratch &scratch, Ext4AllocatedEmptyDataPlan &output,
        Ext4AllocatedEmptyDataWitness &witness)
    {
        using namespace ext4_checked;
        for (const auto *home : {&after[0], &after[1], &after[2], &after[3]})
            if (overlap(scratch, *home) || overlap(output, *home) || overlap(witness, *home))
                return Status::invalid_argument("allocated empty witness after/workspace overlap");
        if (overlap(scratch, output) || overlap(scratch, witness) || overlap(output, witness) ||
            overlap(scratch, binding) || overlap(scratch, descriptor) || overlap(scratch, inode_bitmap) ||
            overlap(output, binding) || overlap(output, descriptor) || overlap(output, inode_bitmap) ||
            overlap(witness, binding) || overlap(witness, descriptor) || overlap(witness, inode_bitmap))
            return Status::invalid_argument("allocated empty witness decode workspace overlap");
        scratch = {}; output = {}; witness = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("allocated empty witness absent or edition unknown");
        if (le16(descriptor, 138) != length || descriptor[140] != 4 || descriptor[141] || le16(descriptor, 142))
            return Status::io_error("allocated empty witness fixed-length or reserved fields");
        for (usize at = length; at < end; ++at)
            if (descriptor[at]) return Status::io_error("allocated empty witness trailing bytes");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (descriptor[144 + index] != binding.uuid[index])
                return Status::io_error("allocated empty witness UUID");
        const u32 sequence = le32(descriptor, 160);
        if (!sequence || jbd2::detail::be32(descriptor, 0) != jbd2::magic ||
            jbd2::detail::be32(descriptor, 4) != u32(jbd2::RecordType::descriptor) ||
            jbd2::detail::be32(descriptor, 8) != sequence)
            return Status::io_error("allocated empty witness descriptor sequence");
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        scratch.block = descriptor; store32(scratch.block, 1020, 0);
        if (jbd2::detail::be32(descriptor, 1020) != jbd2::detail::crc32c(seed, scratch.block))
            return Status::io_error("allocated empty descriptor checksum");
        if (auto status = check_tags(binding, descriptor, after, scratch); !status) return status;
        auto &p = scratch.reconstructed;
        p.update = {le32(descriptor, 164), le32(descriptor, 168), le32(descriptor, 172),
            le32(descriptor, 184), le32(descriptor, 188)};
        p.group = le32(descriptor, 176); p.physical = le32(descriptor, 180); p.count = 4;
        for (usize home = 0; home < 4; ++home)
        { p.targets[home] = le32(descriptor, 192 + home * 4); p.before[home] = p.after[home] = after[home]; }
        Parent::Geometry geometry{};
        if (auto status = Parent::inspect(binding, after[2], after[1], geometry); !status) return status;
        if (p.update.inode < geometry.first_inode || p.update.inode > geometry.inodes ||
            p.group >= geometry.groups || !Parent::ordinary_block(binding, p.physical) ||
            (p.physical - 1) / geometry.blocks_per_group != p.group)
            return Status::io_error("allocated empty witnessed inode or data group");
        const u32 inode_group = (p.update.inode - 1) / geometry.inodes_per_group;
        const u32 local = (p.update.inode - 1) % geometry.inodes_per_group;
        const usize offset = usize(local % 4) * 256, data_at = usize(p.group) * 32;
        if (p.targets != std::array<u32, 4>{le32(after[1], data_at), 2, 1,
                le32(after[1], inode_group * 32 + 8) + local / 4})
            return Status::io_error("allocated empty witnessed home placement");
        for (usize index = 0; index < 256; ++index)
            p.before[3][offset + index] = descriptor[inode_begin + index];
        const auto old_inode = std::span<const u8>{p.before[3]}.subspan(offset, 256);
        p.old_version = u64{le32(old_inode, 36)} | (u64{le32(old_inode, 152)} << 32);
        if (p.old_version == ~u64{0}) return Status::overflow("allocated empty witnessed version exhausted");
        p.new_version = p.old_version + 1;
        const u32 bit = (p.physical - 1) % geometry.blocks_per_group;
        const u8 mask = u8(1U << (bit % 8));
        if (!(p.before[0][bit / 8] & mask))
            return Status::io_error("allocated empty candidate is free in after image");
        p.before[0][bit / 8] &= u8(~mask);
        const u16 free = le16(after[1], data_at + 12);
        const u32 total = le32(after[2], 12);
        if (free == 0xffffU || total == 0xffffffffU)
            return Status::overflow("allocated empty inverse free counter exhausted");
        auto d = std::span<u8>{p.before[1]}.subspan(data_at, 32);
        store16(d, 12, u16(free + 1));
        store16(d, 24, u16(jbd2::detail::crc32c(seed,
            std::span<const u8>{p.before[0]}.first(geometry.blocks_per_group / 8))));
        store16(d, 30, 0); store16(d, 30, Parent::descriptor_checksum(seed, p.group, d));
        store32(p.before[2], 12, total + 1);
        store32(p.before[2], 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{p.before[2]}.first(1020)));
        for (usize home = 0; home < 4; ++home)
            if (le32(descriptor, 208 + home * 4) != jbd2::detail::crc32c(~u32{0}, p.before[home]))
                return Status::io_error("allocated empty whole before-home checksum");
        if (auto status = regenerate(binding, inode_bitmap, scratch); !status) return status;
        output = p; witness.update = p.update; witness.uuid = binding.uuid; witness.targets = p.targets;
        witness.group = p.group; witness.physical = p.physical; witness.sequence = sequence;
        witness.old_version = p.old_version; witness.new_version = p.new_version;
        return {};
    }
};

// Exact mixed-home classification is a fact, independent of journal commit,
// ownership proof, writer exclusion and any eventual replay authorization.
class Ext4AllocatedEmptyDataHomeMatcher final
{
  public:
    [[nodiscard]] static Status classify(const Ext4AllocatedEmptyDataPlan &plan,
        std::span<const Ext4FirstDataBlock, 4> homes, u8 &after_mask)
    {
        const auto result = reinterpret_cast<usize>(&after_mask);
        const auto overlaps_result = [result](const void *input, usize size)
        { const auto begin = reinterpret_cast<usize>(input); return result >= begin && result - begin < size; };
        if (overlaps_result(&plan, sizeof(plan)) || overlaps_result(homes.data(), homes.size_bytes()))
            return Status::invalid_argument("allocated empty mixed-home result/input overlap");
        after_mask = 0;
        if (plan.count != 4 || !plan.update.size || plan.update.size > 1024 ||
            plan.old_version == ~u64{0} || plan.new_version != plan.old_version + 1)
            return Status::invalid_argument("allocated empty mixed-home plan shape");
        u8 proposed{};
        for (usize home = 0; home < 4; ++home)
        {
            if (plan.before[home] == plan.after[home])
                return Status::invalid_argument("allocated empty mixed-home unchanged participant");
            if (homes[home] == plan.after[home]) proposed |= u8(1U << home);
            else if (homes[home] != plan.before[home])
                return Status::io_error("allocated empty home is neither exact old nor exact after");
        }
        after_mask = proposed;
        return {};
    }
};
} // namespace ok::fs
