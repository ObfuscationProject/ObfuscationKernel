#pragma once

#include "ok/fs/ext4_orphan_chain.hpp"

namespace ok::fs
{
// Initialized groups supply all 512 inode-table blocks in descriptor order.
// An uninitialized group supplies no table blocks and a zero inode bitmap.
struct Ext4OrphanOwnershipSource
{
    Ext4OrphanChainSource chain;
    std::array<std::span<const Ext4OrphanBlock>, 4> inode_tables{};
};

struct Ext4OrphanOwnershipScratch
{
    struct Range { u32 first{}, count{}, inode{}; };
    static constexpr usize inode_capacity = 128;
    static constexpr usize range_capacity = inode_capacity * 4;
    std::array<Range, range_capacity> ranges{};
    std::array<bool, Ext4OrphanChainProof::capacity> seen{};
    usize range_count{}, inode_count{};
};

// A result about one complete set of supplied images, not reclaim authority.
struct Ext4OrphanOwnershipProof
{
    std::array<u8, 16> filesystem_uuid{};
    u32 observed_head{};
    usize orphan_count{}, allocated_inodes{}, ordinary_ranges{}, owned_blocks{};
    bool valid{};
};

class Ext4OrphanOwnershipValidator final
{
    using Block = Ext4OrphanBlock;
    using Range = Ext4OrphanOwnershipScratch::Range;

    [[nodiscard]] static bool overlaps(u32 first, u32 count, u32 other, u32 length)
    {
        return first < u64{other} + length && other < u64{first} + count;
    }
    [[nodiscard]] static bool in_range(u32 block, u32 first, u32 count)
    { return block >= first && u64{block} - first < count; }
    [[nodiscard]] static bool metadata(const Ext4RecoveryInfo &binding, u32 block)
    {
        for (usize index = 0; index < binding.metadata_count; ++index)
            if (in_range(block, binding.metadata[index].first, binding.metadata[index].count))
                return true;
        return false;
    }
    [[nodiscard]] static bool journal(const Ext4RecoveryInfo &binding, u32 block)
    { return in_range(block, u32(binding.layout.journal_first_block), u32(binding.layout.journal_blocks)); }
    [[nodiscard]] static bool bit(const Block &bitmap, u32 local)
    { return bitmap[local / 8] & (1U << (local % 8)); }

    [[nodiscard]] static Status reserved(u32 seed, u32 number, std::span<const u8> inode)
    {
        if (jbd2::detail::all_zero(inode)) return {};
        for (usize at = 0; at < inode.size(); ++at)
            if (!(at >= 8 && at < 20) && at != 124 && at != 125 && inode[at])
                return Status::unsupported("orphan ownership opaque reserved inode");
        auto crc = ext4_checked::inode_seed(seed, number, inode);
        for (usize at = 0; at < inode.size(); ++at)
            crc = jbd2::detail::crc_byte(crc, at == 124 || at == 125 ? 0 : inode[at]);
        return u16(crc) == ext4_checked::le16(inode, 124) ? Status{} :
            Status::io_error("orphan ownership reserved inode checksum");
    }

    [[nodiscard]] static Status append(const Ext4RecoveryInfo &binding,
                                       const Ext4OrphanOwnershipSource &source,
                                       Ext4OrphanOwnershipScratch &scratch,
                                       u32 number, u32 first, u32 count)
    {
        if (!count || first < 1 || first >= binding.layout.filesystem_blocks ||
            count > binding.layout.filesystem_blocks - first)
            return Status::unsupported("orphan ownership extent bounds");
        for (u32 part = 0; part < count; ++part)
        {
            const u32 physical = first + part;
            if (metadata(binding, physical) || journal(binding, physical))
                return Status::io_error("orphan ownership extent aliases reserved block");
            const u32 group = (physical - 1) / 8192, local = (physical - 1) % 8192;
            if (ext4_checked::le16(source.chain.descriptors, group * 32 + 18) & 2)
                return Status::unsupported("orphan ownership extent in uninitialized block group");
            if (!bit(source.chain.block_bitmaps[group], local))
                return Status::io_error("orphan ownership extent block is unallocated");
        }
        for (usize prior = 0; prior < scratch.range_count; ++prior)
            if (overlaps(first, count, scratch.ranges[prior].first,
                         scratch.ranges[prior].count))
                return Status::io_error("orphan ownership physical extents overlap");
        if (scratch.range_count == scratch.ranges.size())
            return Status::no_memory("orphan ownership range budget");
        scratch.ranges[scratch.range_count++] = {first, count, number};
        return {};
    }

    [[nodiscard]] static Status journal_inode(const Ext4RecoveryInfo &binding, u32 seed,
                                               std::span<const u8> inode)
    {
        using namespace ext4_checked;
        if (auto status = inode_checksum(seed, 8, inode); !status) return status;
        const u32 count = u32(binding.layout.journal_blocks);
        if (count > 32768 || u64{count} * 1024 > 0xffffffffULL ||
            (le16(inode, 0) & 0170000) != 0100000 || le16(inode, 26) != 1 ||
            le32(inode, 4) != count * 1024 || le32(inode, 28) != count * 2 ||
            le32(inode, 32) != 0x80000 || le16(inode, 40) != 0xf30a ||
            le16(inode, 42) != 1 || le16(inode, 44) != 4 || le16(inode, 46) ||
            le32(inode, 52) || le16(inode, 56) != count || le16(inode, 58) ||
            le32(inode, 60) != binding.layout.journal_first_block ||
            le32(inode, 20) || le32(inode, 104) || le32(inode, 108) ||
            le32(inode, 112) || le16(inode, 116) || le16(inode, 118) ||
            !jbd2::detail::all_zero(inode.subspan(64, 36)))
            return Status::unsupported("orphan ownership journal inode mapping differs");
        return {};
    }

    [[nodiscard]] static Status ordinary_inode(const Ext4RecoveryInfo &binding,
                                               const Ext4OrphanOwnershipSource &source,
                                               const Ext4OrphanChainProof &chain,
                                               Ext4OrphanOwnershipScratch &scratch,
                                               u32 seed, u32 number,
                                               std::span<const u8> inode)
    {
        using namespace ext4_checked;
        if (auto status = inode_checksum(seed, number, inode); !status) return status;
        const u16 type = le16(inode, 0) & 0170000;
        const u32 size = le32(inode, 4), generation = le32(inode, 100);
        const auto *member = chain.member(number, generation);
        if (type == 0120000)
        {
            // Linked fast symlinks own only their checked inline target bytes.
            // They never become orphan members or contribute physical ranges.
            if (member || !le16(inode, 26) || le32(inode, 20) || !size || size >= 60 ||
                le32(inode, 28) || le32(inode, 32) || le32(inode, 104) ||
                le32(inode, 108) || le32(inode, 112) || le16(inode, 116) ||
                le16(inode, 118) || le16(inode, 126) || le32(inode, 156))
                return Status::unsupported("orphan ownership linked fast symlink profile");
            for (usize at = 40; at < 40 + size; ++at)
                if (!inode[at])
                    return Status::unsupported("orphan ownership fast symlink target contains zero");
            for (usize at = 40 + size; at < 100; ++at)
                if (inode[at])
                    return Status::unsupported("orphan ownership fast symlink unused target bytes");
            for (usize at = 160; at < inode.size(); ++at)
                if (inode[at])
                    return Status::unsupported("orphan ownership fast symlink extra fields");
            return {};
        }
        // The reclaim victim and directory budgets remain unchanged. A linked
        // regular nonmember may map larger initialized inline extents, bounded
        // by the complete checked 32 MiB filesystem and exact owner bitmap.
        const u32 block_limit = member || type == 0040000 ? 64U :
            u32(binding.layout.filesystem_blocks);
        if ((type != 0100000 && type != 0040000) || u64{size} > u64{block_limit} * 1024 ||
            le32(inode, 32) != 0x80000 || le16(inode, 40) != 0xf30a ||
            le16(inode, 42) > 4 || le16(inode, 44) != 4 || le16(inode, 46) ||
            le32(inode, 48) || le32(inode, 104) || le32(inode, 108) ||
            le32(inode, 112) || le16(inode, 116) || le16(inode, 118) ||
            le16(inode, 126) || le32(inode, 156) ||
            (member && (type != 0100000 || le16(inode, 26) ||
                        size != member->size || le32(inode, 20) != member->next)) ||
            (!member && (!le16(inode, 26) || le32(inode, 20))))
            return Status::unsupported("orphan ownership ordinary inode profile");
        for (usize at = 160; at < inode.size(); ++at)
            if (inode[at]) return Status::unsupported("orphan ownership inode extra fields");
        for (usize at = 52 + le16(inode, 42) * 12; at < 100; ++at)
            if (inode[at]) return Status::unsupported("orphan ownership unused extent bytes");
        const u32 logical_blocks = u32((u64{size} + 1023) / 1024);
        u32 next_logical = 0, mapped = 0, matched = 0;
        for (u32 record = 0; record < le16(inode, 42); ++record)
        {
            const usize at = 52 + record * 12;
            const u32 logical = le32(inode, at), length = le16(inode, at + 4);
            const u32 physical = le32(inode, at + 8);
            if (!length || length > 32768 || logical < next_logical ||
                logical >= logical_blocks || length > logical_blocks - logical ||
                le16(inode, at + 6) || !physical || length > block_limit - mapped ||
                (member && logical != next_logical))
                return Status::unsupported("orphan ownership unsupported inline extent");
            if (auto status = append(binding, source, scratch, number, physical, length); !status)
                return status;
            if (member)
                for (u32 part = 0; part < length; ++part)
                    if (matched >= member->data_count ||
                        physical + part != member->data_blocks[matched++])
                        return Status::io_error("orphan ownership chain mapping differs");
            next_logical = logical + length;
            mapped += length;
        }
        if (le32(inode, 28) != mapped * 2 ||
            (type == 0040000 && mapped != logical_blocks) ||
            (member && (matched != member->data_count || mapped != logical_blocks)) ||
            (!size && le16(inode, 42)))
            return Status::unsupported("orphan ownership mapped-sector coverage");
        if (member)
        {
            bool found = false;
            for (usize index = 0; index < chain.count(); ++index)
                if (chain.at(index)->number == number &&
                    chain.at(index)->generation == generation)
                {
                    if (scratch.seen[index])
                        return Status::io_error("orphan ownership repeated chain member");
                    scratch.seen[index] = true; found = true; break;
                }
            if (!found) return Status::fault("orphan ownership member indexing");
        }
        return {};
    }

  public:
    [[nodiscard]] static Status validate(const Ext4RecoveryInfo &binding,
                                         const Ext4OrphanOwnershipSource &source,
                                         Ext4OrphanOwnershipScratch &scratch,
                                         Ext4OrphanOwnershipProof &output)
    {
        using namespace ext4_checked;
        output = {}; scratch = {};
        Ext4OrphanChainProof chain{};
        if (auto status = Ext4OrphanChainValidator::validate(binding, source.chain, chain); !status)
            return status;
        if (!chain.count())
            return Status::invalid_argument("orphan ownership requires a nonempty chain");
        if (binding.layout.filesystem_blocks != 32768 || binding.layout.block_size != 1024 ||
            binding.metadata_count != 16 || binding.layout.journal_first_block > 32767 ||
            binding.layout.journal_blocks > 32768 ||
            le32(source.chain.superblock, 336) || le32(source.chain.superblock, 340) ||
            le32(source.chain.superblock, 344))
            return Status::unsupported("orphan ownership bounded geometry");
        for (usize one = 0; one < binding.metadata_count; ++one)
        {
            const auto current = binding.metadata[one];
            if (!current.count || overlaps(u32(current.first), u32(current.count),
                                           u32(binding.layout.journal_first_block),
                                           u32(binding.layout.journal_blocks)))
                return Status::unsupported("orphan ownership reserved intervals overlap");
            for (usize earlier = 0; earlier < one; ++earlier)
                if (overlaps(u32(current.first), u32(current.count),
                             u32(binding.metadata[earlier].first),
                             u32(binding.metadata[earlier].count)))
                    return Status::unsupported("orphan ownership metadata intervals overlap");
        }
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        u32 free_blocks = 0;
        for (u32 group = 0; group < 4; ++group)
        {
            const auto descriptor = std::span<const u8>{source.chain.descriptors}.subspan(group * 32, 32);
            const u16 flags = le16(descriptor, 18);
            const auto &inode_bitmap = source.chain.inode_bitmaps[group];
            const auto &block_bitmap = source.chain.block_bitmaps[group];
            const u32 blocks = group == 3 ? 8191 : 8192;
            free_blocks += le16(descriptor, 12);
            if (flags & 1)
            {
                if (!source.inode_tables[group].empty())
                    return Status::invalid_argument("orphan ownership uninitialized table supplied");
                for (usize index = 0; index < 256; ++index)
                    if (inode_bitmap[index])
                        return Status::unsupported("orphan ownership uninitialized inode bitmap has bits");
            }
            else
            {
                if (source.inode_tables[group].size() != 512)
                    return Status::invalid_argument("orphan ownership incomplete inode table");
                if (u16(jbd2::detail::crc32c(seed, std::span<const u8>{inode_bitmap}.first(256))) !=
                    le16(descriptor, 26))
                    return Status::io_error("orphan ownership inode bitmap checksum");
                u32 allocated = 0;
                for (u32 local = 0; local < 2048; ++local)
                    allocated += bit(inode_bitmap, local);
                if (allocated != 2048U - le16(descriptor, 14))
                    return Status::io_error("orphan ownership inode bitmap count");
            }
            if (flags & 2)
            {
                if (overlaps(1 + group * 8192, blocks,
                             u32(binding.layout.journal_first_block),
                             u32(binding.layout.journal_blocks)))
                    return Status::unsupported("orphan ownership journal bitmap is uninitialized");
                for (u8 byte : block_bitmap)
                    if (byte) return Status::unsupported("orphan ownership uninitialized block bitmap has bits");
            }
            else
            {
                if (u16(jbd2::detail::crc32c(seed, block_bitmap)) != le16(descriptor, 24))
                    return Status::io_error("orphan ownership block bitmap checksum");
                u32 allocated = 0;
                for (u32 local = 0; local < blocks; ++local)
                    allocated += bit(block_bitmap, local);
                if (allocated != blocks - le16(descriptor, 12))
                    return Status::io_error("orphan ownership block bitmap count");
            }
        }
        if (free_blocks != le32(source.chain.superblock, 12))
            return Status::io_error("orphan ownership free block counters differ");
        for (u32 number = 1; number <= 10; ++number)
            if (!bit(source.chain.inode_bitmaps[0], number - 1))
                return Status::unsupported("orphan ownership reserved inode is unallocated");
        for (const auto &image : source.chain.chain)
        {
            const u32 group = (image.number - 1) / 2048;
            const u32 local = (image.number - 1) % 2048;
            const u32 table = le32(source.chain.descriptors, group * 32 + 8);
            if (source.inode_tables[group].size() != 512 ||
                image.table_block != table + local / 4 ||
                source.inode_tables[group][local / 4] != image.table)
                return Status::io_error("orphan ownership chain and complete table differ");
        }
        bool journal_seen = false;
        for (u32 group = 0; group < 4; ++group)
        {
            if (source.inode_tables[group].empty()) continue;
            const auto &bitmap = source.chain.inode_bitmaps[group];
            for (u32 local = 0; local < 2048; ++local)
            {
                if (!bit(bitmap, local)) continue;
                if (++scratch.inode_count > scratch.inode_capacity)
                    return Status::no_memory("orphan ownership allocated inode budget");
                const u32 number = group * 2048 + local + 1;
                const auto &table = source.inode_tables[group][local / 4];
                const auto inode = std::span<const u8>{table}.subspan((local % 4) * 256, 256);
                if (number == 8)
                {
                    if (auto status = journal_inode(binding, seed, inode); !status) return status;
                    journal_seen = true;
                }
                else if (number != 2 && number < 11)
                {
                    if (auto status = reserved(seed, number, inode); !status) return status;
                }
                else if (auto status = ordinary_inode(binding, source, chain, scratch,
                                                      seed, number, inode); !status)
                    return status;
            }
        }
        if (!journal_seen)
            return Status::io_error("orphan ownership journal inode is absent");
        for (usize index = 0; index < chain.count(); ++index)
            if (!scratch.seen[index])
                return Status::io_error("orphan ownership chain member was not scanned");
        usize blocks_owned = 0;
        for (u32 group = 0; group < 4; ++group)
        {
            const u16 flags = le16(source.chain.descriptors, group * 32 + 18);
            if (flags & 2) continue;
            const u32 blocks = group == 3 ? 8191 : 8192;
            for (u32 local = 0; local < blocks; ++local)
            {
                const u32 physical = 1 + group * 8192 + local;
                usize ordinary_owners = 0;
                for (usize index = 0; index < scratch.range_count; ++index)
                    ordinary_owners += in_range(physical, scratch.ranges[index].first,
                                                scratch.ranges[index].count);
                const usize reserved_owners = metadata(binding, physical) + journal(binding, physical);
                if (reserved_owners + ordinary_owners > 1 ||
                    bit(source.chain.block_bitmaps[group], local) !=
                        bool(reserved_owners + ordinary_owners))
                    return Status::io_error("orphan ownership exact block bitmap owner differs");
                blocks_owned += ordinary_owners;
            }
        }
        output.filesystem_uuid = chain.filesystem_uuid();
        output.observed_head = chain.observed_head();
        output.orphan_count = chain.count();
        output.allocated_inodes = scratch.inode_count;
        output.ordinary_ranges = scratch.range_count;
        output.owned_blocks = blocks_owned;
        output.valid = true;
        return {};
    }
};
} // namespace ok::fs
