#pragma once

#include "ok/fs/ext4_orphan_reclaim_plan.hpp"

namespace ok::fs
{
struct Ext4OrphanReclaimWitness
{
    std::array<u8, 16> uuid{};
    std::array<u32, Ext4OrphanReclaimPlan::capacity> targets{};
    std::array<u32, 16> released_mapping{};
    u32 inode{}, old_generation{}, retained_generation{}, next_head{}, sequence{};
    usize count{}, released_blocks{}, runs{}, bytes{};
};

// Private descriptor-padding extension. It reconstructs exact old bytes but
// supplies neither device/session provenance nor permission to replay or free.
class Ext4OrphanReclaimWitnessCodec final
{
    using Block = Ext4OrphanBlock;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'O', 'R', 'W', '0', '0', '1'};
    static void store16(Block &block, usize at, u16 value)
    { block[at] = u8(value); block[at + 1] = u8(value >> 8); }
    static void store32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (8 * i)); }
    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 group,
                                                  std::span<const u8> descriptor)
    {
        std::array<u8, 32> copy{};
        for (usize i = 0; i < copy.size(); ++i) copy[i] = descriptor[i];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), copy));
    }
    [[nodiscard]] static bool same_uuid(const Block &block, const std::array<u8, 16> &uuid)
    {
        for (usize i = 0; i < uuid.size(); ++i)
            if (block[104 + i] != uuid[i]) return false;
        return true;
    }
    [[nodiscard]] static Status mapped_inode(const std::array<u8, 16> &uuid,
        std::span<const u8> inode_bytes, u32 inode, u32 next,
        usize released, std::span<const u32> mapping)
    {
        using namespace ext4_checked;
        const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid);
        if (auto status = inode_checksum(seed, inode, inode_bytes); !status) return status;
        const u32 size = le32(inode_bytes, 4);
        const usize blocks = usize((u64{size} + 1023) / 1024);
        const usize records = le16(inode_bytes, 42);
        if (blocks != released || released > 16 || mapping.size() < released ||
            le32(inode_bytes, 20) != next ||
            (le16(inode_bytes, 0) & 0170000) != 0100000 ||
            le16(inode_bytes, 26) || le32(inode_bytes, 28) != blocks * 2 ||
            le32(inode_bytes, 32) != 0x80000 ||
            le16(inode_bytes, 40) != 0xf30a || records > 4 ||
            le16(inode_bytes, 44) != 4 || le16(inode_bytes, 46) ||
            le32(inode_bytes, 48) || (!blocks && records))
            return Status::io_error("orphan reclaim witnessed old extent profile");
        usize mapped = 0;
        for (usize record = 0; record < records; ++record)
        {
            const usize at = 52 + record * 12;
            const u32 logical = le32(inode_bytes, at);
            const usize length = le16(inode_bytes, at + 4);
            const u32 physical = le32(inode_bytes, at + 8);
            if (!length || length > released - mapped || logical != mapped ||
                le16(inode_bytes, at + 6) || !physical ||
                physical > 32768 - length)
                return Status::io_error("orphan reclaim witnessed extent shape");
            for (usize part = 0; part < length; ++part)
                if (mapping[mapped++] != physical + part)
                    return Status::io_error("orphan reclaim witnessed released mapping");
        }
        return mapped == released ? Status{} :
            Status::io_error("orphan reclaim witnessed extent coverage");
    }
    [[nodiscard]] static Status identity(const Ext4RecoveryInfo &binding,
                                          const Ext4OrphanReclaimPlan &plan)
    {
        using namespace ext4_checked;
        if ((plan.count != 4 && plan.count != 5) || plan.count != (plan.released_blocks ? 5U : 4U) ||
            plan.released_blocks > plan.released_mapping.size() ||
            plan.inode < 11 || plan.inode > 8192 ||
            plan.old_generation == ~u32{0} || plan.retained_generation != plan.old_generation + 1 ||
            plan.next_head > 8192 || plan.next_head == plan.inode)
            return Status::invalid_argument("orphan reclaim witness identity");
        for (usize i = plan.released_blocks; i < plan.released_mapping.size(); ++i)
            if (plan.released_mapping[i])
                return Status::invalid_argument("orphan reclaim unused mapping");
        for (usize i = 0; i < plan.count; ++i)
        {
            if (!plan.targets[i] || plan.targets[i] >= binding.layout.filesystem_blocks ||
                plan.before[i] == plan.after[i])
                return Status::invalid_argument("orphan reclaim witness home");
            for (usize prior = 0; prior < i; ++prior)
                if (plan.targets[prior] == plan.targets[i])
                    return Status::invalid_argument("orphan reclaim duplicate home");
        }
        if (plan.targets[plan.count - 2] != 2 || plan.targets[plan.count - 1] != 1 ||
            !same_uuid(plan.before[plan.count - 1], binding.uuid) ||
            !same_uuid(plan.after[plan.count - 1], binding.uuid) ||
            le32(plan.before[plan.count - 1], 232) != plan.inode ||
            le32(plan.after[plan.count - 1], 232) != plan.next_head)
            return Status::invalid_argument("orphan reclaim superblock binding");
        const u32 group = (plan.inode - 1) / 2048;
        const u32 local = (plan.inode - 1) % 2048;
        const auto &descriptors = plan.before[plan.count - 2];
        if (plan.targets[0] != le32(descriptors, group * 32 + 8) + local / 4 ||
            plan.targets[1] != le32(descriptors, group * 32 + 4))
            return Status::invalid_argument("orphan reclaim table or inode bitmap target");
        const auto old_inode = std::span<const u8>{plan.before[0]}.subspan((local % 4) * 256, 256);
        const auto new_inode = std::span<const u8>{plan.after[0]}.subspan((local % 4) * 256, 256);
        if (le32(old_inode, 100) != plan.old_generation ||
            le32(new_inode, 100) != plan.retained_generation ||
            le32(new_inode, 160) != 0x3146524fU)
            return Status::invalid_argument("orphan reclaim inode generation");
        if (auto status = mapped_inode(binding.uuid, old_inode, plan.inode, plan.next_head,
                plan.released_blocks, plan.released_mapping); !status)
            return status;
        for (usize i = 0; i < 1024; ++i)
            if ((i < (local % 4) * 256 || i >= (local % 4 + 1) * 256) &&
                plan.before[0][i] != plan.after[0][i])
                return Status::invalid_argument("orphan reclaim sibling changed");
        Block bitmap = plan.before[1];
        const u32 inode_byte = local / 8;
        const u8 inode_mask = u8(1U << (local % 8));
        if (!(bitmap[inode_byte] & inode_mask))
            return Status::invalid_argument("orphan reclaim inode was already free");
        bitmap[inode_byte] &= u8(~inode_mask);
        if (bitmap != plan.after[1])
            return Status::invalid_argument("orphan reclaim inode bitmap delta");
        if (plan.released_blocks)
        {
            const u32 first = plan.released_mapping[0];
            if (!first) return Status::invalid_argument("orphan reclaim zero data block");
            const u32 data_group = (first - 1) / 8192;
            if (data_group >= 4 ||
                plan.targets[2] != le32(descriptors, data_group * 32))
                return Status::invalid_argument("orphan reclaim block bitmap target");
            bitmap = plan.before[2];
            for (usize i = 0; i < plan.released_blocks; ++i)
            {
                const u32 block = plan.released_mapping[i];
                if (!block || block >= binding.layout.filesystem_blocks ||
                    (block - 1) / 8192 != data_group)
                    return Status::invalid_argument("orphan reclaim released mapping");
                for (usize prior = 0; prior < i; ++prior)
                    if (plan.released_mapping[prior] == block)
                        return Status::invalid_argument("orphan reclaim duplicate released block");
                const u32 bit = (block - 1) % 8192;
                const u8 mask = u8(1U << (bit % 8));
                if (!(bitmap[bit / 8] & mask))
                    return Status::invalid_argument("orphan reclaim block was already free");
                bitmap[bit / 8] &= u8(~mask);
            }
            if (bitmap != plan.after[2])
                return Status::invalid_argument("orphan reclaim block bitmap delta");
        }
        return {};
    }
    [[nodiscard]] static Status decoded_identity(const Block &descriptor,
        const std::array<u8, 16> &uuid, std::span<const Block> after,
        std::span<const Block> before, usize count, usize released)
    {
        using namespace ext4_checked;
        const u32 inode = le32(descriptor, 160), generation = le32(descriptor, 164);
        const u32 retained = le32(descriptor, 168), next = le32(descriptor, 172);
        if (inode < 11 || inode > 8192 || generation == ~u32{0} ||
            retained != generation + 1 || next > 8192 || next == inode ||
            le32(before[count - 1], 232) != inode ||
            le32(after[count - 1], 232) != next ||
            !same_uuid(before[count - 1], uuid) ||
            !same_uuid(after[count - 1], uuid) ||
            le32(descriptor, 180 + (count - 2) * 4) != 2 ||
            le32(descriptor, 180 + (count - 1) * 4) != 1)
            return Status::io_error("orphan reclaim decoded identity");
        const u32 group = (inode - 1) / 2048, local = (inode - 1) % 2048;
        const auto &descriptors = before[count - 2];
        if (le32(descriptor, 180) != le32(descriptors, group * 32 + 8) + local / 4 ||
            le32(descriptor, 184) != le32(descriptors, group * 32 + 4))
            return Status::io_error("orphan reclaim decoded home targets");
        const auto old_inode = std::span<const u8>{before[0]}.subspan((local % 4) * 256, 256);
        const auto new_inode = std::span<const u8>{after[0]}.subspan((local % 4) * 256, 256);
        if (le32(old_inode, 100) != generation || le32(new_inode, 100) != retained ||
            le32(new_inode, 160) != 0x3146524fU)
            return Status::io_error("orphan reclaim decoded inode generation");
        std::array<u32, 16> mapping{};
        for (usize i = 0; i < released; ++i)
            mapping[i] = le32(descriptor, 240 + i * 4);
        if (auto status = mapped_inode(uuid, old_inode, inode, next, released, mapping); !status)
            return status;
        for (usize i = 0; i < 1024; ++i)
            if ((i < (local % 4) * 256 || i >= (local % 4 + 1) * 256) &&
                before[0][i] != after[0][i])
                return Status::io_error("orphan reclaim decoded sibling changed");
        Block bitmap = before[1];
        const u8 inode_mask = u8(1U << (local % 8));
        if (!(bitmap[local / 8] & inode_mask))
            return Status::io_error("orphan reclaim decoded inode already free");
        bitmap[local / 8] &= u8(~inode_mask);
        if (bitmap != after[1])
            return Status::io_error("orphan reclaim decoded inode bitmap");
        u32 data_group = 0;
        if (released)
        {
            const u32 first = le32(descriptor, 240);
            if (!first) return Status::io_error("orphan reclaim decoded zero block");
            data_group = (first - 1) / 8192;
            if (data_group >= 4 ||
                le32(descriptor, 188) != le32(descriptors, data_group * 32))
                return Status::io_error("orphan reclaim decoded block bitmap target");
            bitmap = before[2];
            for (usize i = 0; i < released; ++i)
            {
                const u32 block = le32(descriptor, 240 + i * 4);
                if (!block || (block - 1) / 8192 != data_group)
                    return Status::io_error("orphan reclaim decoded released mapping");
                for (usize prior = 0; prior < i; ++prior)
                    if (le32(descriptor, 240 + prior * 4) == block)
                        return Status::io_error("orphan reclaim decoded duplicate block");
                const u32 bit = (block - 1) % 8192;
                const u8 mask = u8(1U << (bit % 8));
                if (!(bitmap[bit / 8] & mask))
                    return Status::io_error("orphan reclaim decoded block already free");
                bitmap[bit / 8] &= u8(~mask);
            }
            if (bitmap != after[2])
                return Status::io_error("orphan reclaim decoded block bitmap delta");
        }
        const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid);
        Block expected = before[0];
        auto slot = std::span<u8>{expected}.subspan((local % 4) * 256, 256);
        for (u8 &byte : slot) byte = 0;
        for (usize i = 0; i < 4; ++i) slot[100 + i] = u8(retained >> (8 * i));
        slot[128] = 32;
        slot[160] = 'O'; slot[161] = 'R'; slot[162] = 'F'; slot[163] = '1';
        const u32 inode_crc = jbd2::detail::crc32c(
            ext4_checked::inode_seed(seed, inode, slot), slot);
        slot[124] = u8(inode_crc); slot[125] = u8(inode_crc >> 8);
        slot[130] = u8(inode_crc >> 16); slot[131] = u8(inode_crc >> 24);
        if (expected != after[0])
            return Status::io_error("orphan reclaim decoded table image");

        const u16 free_inodes = le16(descriptors, group * 32 + 14);
        const u32 super_free_inodes = le32(before[count - 1], 16);
        const u16 free_blocks = released ? le16(descriptors, data_group * 32 + 12) : 0;
        const u32 super_free_blocks = le32(before[count - 1], 12);
        if (free_inodes >= 2048 || super_free_inodes >= 8192 ||
            (released && (u32{free_blocks} + released >
                (data_group == 3 ? 8191U : 8192U) ||
                super_free_blocks > 32767U - released)))
            return Status::io_error("orphan reclaim decoded free counter overflow");
        expected = descriptors;
        store16(expected, group * 32 + 14, u16(free_inodes + 1));
        store16(expected, group * 32 + 26, u16(jbd2::detail::crc32c(
            seed, std::span<const u8>{after[1]}.first(256))));
        if (released)
        {
            store16(expected, data_group * 32 + 12, u16(free_blocks + released));
            store16(expected, data_group * 32 + 24,
                u16(jbd2::detail::crc32c(seed, after[2])));
        }
        store16(expected, group * 32 + 30, 0);
        store16(expected, group * 32 + 30,
            descriptor_checksum(seed, group,
                std::span<const u8>{expected}.subspan(group * 32, 32)));
        if (released && data_group != group)
        {
            store16(expected, data_group * 32 + 30, 0);
            store16(expected, data_group * 32 + 30,
                descriptor_checksum(seed, data_group,
                    std::span<const u8>{expected}.subspan(data_group * 32, 32)));
        }
        if (expected != after[count - 2])
            return Status::io_error("orphan reclaim decoded descriptor image");
        expected = before[count - 1];
        store32(expected, 16, super_free_inodes + 1);
        if (released) store32(expected, 12, super_free_blocks + u32(released));
        store32(expected, 232, next);
        store32(expected, 1020, jbd2::detail::crc32c(
            ~u32{0}, std::span<const u8>{expected}.first(1020)));
        if (expected != after[count - 1])
            return Status::io_error("orphan reclaim decoded superblock image");
        return {};
    }

  public:
    static constexpr usize begin = 128, patch_begin = 304, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize i = 0; i < magic.size(); ++i)
            if (descriptor[begin + i] != magic[i]) return false;
        return true;
    }
    // The descriptor is unchanged when validation or full preimage encoding
    // fails. The caller supplies the already checked canonical reclaim plan.
    [[nodiscard]] static Status encode(const Ext4RecoveryInfo &binding,
        const Ext4OrphanReclaimPlan &plan, u32 sequence, Block &descriptor)
    {
        if (auto status = identity(binding, plan); !status) return status;
        if (!sequence || sequence != jbd2::detail::be32(descriptor, 8))
            return Status::invalid_argument("orphan reclaim witness sequence");
        Block proposed = descriptor;
        for (usize at = begin; at < end; ++at) proposed[at] = 0;
        for (usize i = 0; i < magic.size(); ++i) proposed[begin + i] = magic[i];
        store16(proposed, 136, 1);
        proposed[140] = u8(plan.count); proposed[141] = u8(plan.released_blocks);
        for (usize i = 0; i < binding.uuid.size(); ++i) proposed[144 + i] = binding.uuid[i];
        store32(proposed, 160, plan.inode); store32(proposed, 164, plan.old_generation);
        store32(proposed, 168, plan.retained_generation); store32(proposed, 172, plan.next_head);
        store32(proposed, 176, sequence);
        for (usize home = 0; home < plan.count; ++home)
        {
            store32(proposed, 180 + home * 4, plan.targets[home]);
            store32(proposed, 200 + home * 4,
                 jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            store32(proposed, 220 + home * 4,
                 jbd2::detail::crc32c(~u32{0}, plan.after[home]));
        }
        for (usize i = 0; i < plan.released_blocks; ++i)
            store32(proposed, 240 + i * 4, plan.released_mapping[i]);
        usize cursor = patch_begin, runs = 0;
        for (usize home = 0; home < plan.count; ++home)
            for (usize at = 0; at < 1024;)
            {
                if (plan.before[home][at] == plan.after[home][at]) { ++at; continue; }
                const usize first = at;
                while (at < 1024 && plan.before[home][at] != plan.after[home][at]) ++at;
                const usize length = at - first;
                if (cursor > end || end - cursor < 5 || length > end - cursor - 5)
                    return Status::unsupported("orphan reclaim complete witness exceeds descriptor");
                proposed[cursor] = u8(home);
                store16(proposed, cursor + 1, u16(first)); store16(proposed, cursor + 3, u16(length));
                cursor += 5;
                for (usize i = 0; i < length; ++i) proposed[cursor++] = plan.before[home][first + i];
                ++runs;
            }
        store16(proposed, 138, u16(cursor)); store16(proposed, 142, u16(runs));
        descriptor = proposed;
        return {};
    }
    // Decodes only a fully checksummed descriptor and every supplied logical
    // after-image. before may be partially overwritten on error and must be
    // discarded unless success is returned. All buffers must be disjoint.
    [[nodiscard]] static Status decode(const Block &descriptor,
        const std::array<u8, 16> &expected_uuid, std::span<const Block> after,
        std::span<Block> before, Ext4OrphanReclaimWitness &output)
    {
        using namespace ext4_checked;
        output = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("orphan reclaim witness absent or version unknown");
        const usize count = descriptor[140], released = descriptor[141];
        const usize limit = le16(descriptor, 138), runs = le16(descriptor, 142);
        if ((count != 4 && count != 5) || count != (released ? 5U : 4U) ||
            released > 16 || after.size() != count || before.size() != count ||
            limit < patch_begin || limit > end)
            return Status::invalid_argument("orphan reclaim witness geometry");
        for (usize i = 0; i < expected_uuid.size(); ++i)
            if (descriptor[144 + i] != expected_uuid[i])
                return Status::io_error("orphan reclaim witness UUID");
        const u32 sequence = le32(descriptor, 176);
        if (!sequence || jbd2::detail::be32(descriptor, 0) != jbd2::magic ||
            jbd2::detail::be32(descriptor, 4) != u32(jbd2::RecordType::descriptor) ||
            jbd2::detail::be32(descriptor, 8) != sequence)
            return Status::io_error("orphan reclaim witness descriptor sequence");
        const u32 seed = jbd2::detail::crc32c(~u32{0}, expected_uuid);
        Block checked = descriptor;
        for (usize i = 1020; i < 1024; ++i) checked[i] = 0;
        if (jbd2::detail::be32(descriptor, 1020) != jbd2::detail::crc32c(seed, checked))
            return Status::io_error("orphan reclaim descriptor checksum");
        for (usize at = count == 4 ? 92U : 108U; at < begin; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim tag padding");
        for (usize at = limit; at < end; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim witness trailing bytes");
        for (usize at = 180 + count * 4; at < 200; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim unused targets");
        for (usize at = 200 + count * 4; at < 220; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim unused before CRC");
        for (usize at = 220 + count * 4; at < 240; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim unused after CRC");
        for (usize at = 240 + released * 4; at < patch_begin; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim unused mapping");
        for (usize at = 28; at < 44; ++at)
            if (descriptor[at]) return Status::io_error("orphan reclaim first tag UUID");
        const auto sequence_bytes = std::span<const u8>{descriptor}.subspan(8, 4);
        for (usize home = 0; home < count; ++home)
        {
            const u32 target = le32(descriptor, 180 + home * 4);
            const usize at = home ? 44 + (home - 1) * 16 : 12;
            const bool escaped = jbd2::detail::be32(after[home], 0) == jbd2::magic;
            const u32 flags = (home ? jbd2::flag_same_uuid : 0) |
                (home + 1 == count ? jbd2::flag_last_tag : 0) |
                (escaped ? jbd2::flag_escape : 0);
            if (!target || jbd2::detail::be32(descriptor, at) != target ||
                jbd2::detail::be32(descriptor, at + 4) != flags ||
                jbd2::detail::be32(descriptor, at + 8) ||
                le32(descriptor, 220 + home * 4) !=
                    jbd2::detail::crc32c(~u32{0}, after[home]))
                return Status::io_error("orphan reclaim target or whole after-home binding");
            for (usize prior = 0; prior < home; ++prior)
                if (le32(descriptor, 180 + prior * 4) == target)
                    return Status::io_error("orphan reclaim duplicate target");
            checked = after[home];
            if (escaped)
                for (usize i = 0; i < 4; ++i) checked[i] = 0;
            if (jbd2::detail::be32(descriptor, at + 12) !=
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence_bytes), checked))
                return Status::io_error("orphan reclaim payload tag checksum");
            before[home] = after[home];
        }
        usize cursor = patch_begin, prior_home = 0, prior_end = 0;
        for (usize run = 0; run < runs; ++run)
        {
            if (cursor > limit || limit - cursor < 5)
                return Status::io_error("orphan reclaim truncated patch");
            const usize home = descriptor[cursor], offset = le16(descriptor, cursor + 1);
            const usize size = le16(descriptor, cursor + 3);
            cursor += 5;
            if (home >= count || offset >= 1024 || !size || size > 1024 - offset ||
                size > limit - cursor ||
                (run && (home < prior_home || (home == prior_home && offset <= prior_end))))
                return Status::io_error("orphan reclaim patch bounds or order");
            for (usize i = 0; i < size; ++i)
            {
                if (descriptor[cursor + i] == after[home][offset + i])
                    return Status::io_error("orphan reclaim patch includes unchanged byte");
                before[home][offset + i] = descriptor[cursor + i];
            }
            cursor += size; prior_home = home; prior_end = offset + size;
        }
        if (cursor != limit) return Status::io_error("orphan reclaim patch length");
        for (usize home = 0; home < count; ++home)
            if (le32(descriptor, 200 + home * 4) !=
                jbd2::detail::crc32c(~u32{0}, before[home]))
                return Status::io_error("orphan reclaim whole before-home checksum");
        if (auto status = decoded_identity(descriptor, expected_uuid, after,
                std::span<const Block>{before}, count, released); !status)
            return status;
        output.uuid = expected_uuid;
        output.inode = le32(descriptor, 160);
        output.old_generation = le32(descriptor, 164);
        output.retained_generation = le32(descriptor, 168);
        output.next_head = le32(descriptor, 172);
        output.sequence = sequence;
        output.count = count; output.released_blocks = released;
        output.runs = runs; output.bytes = limit - begin;
        for (usize i = 0; i < count; ++i) output.targets[i] = le32(descriptor, 180 + i * 4);
        for (usize i = 0; i < released; ++i)
            output.released_mapping[i] = le32(descriptor, 240 + i * 4);
        return {};
    }
};
} // namespace ok::fs
