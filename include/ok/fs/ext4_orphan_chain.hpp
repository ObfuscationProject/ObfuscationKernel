#pragma once

#include "ok/fs/ext4_readonly.hpp"

namespace ok::fs
{
using Ext4OrphanBlock = std::array<u8, 1024>;

struct Ext4OrphanInodeImage
{
    u32 number{}, expected_generation{}, table_block{};
    Ext4OrphanBlock table{};
};

// All images must come from one owner-exclusive, stable filesystem view. This
// borrowed input does not attest device provenance or authorize any write.
struct Ext4OrphanChainSource
{
    const Ext4OrphanBlock &superblock;
    const Ext4OrphanBlock &descriptors;
    const std::array<Ext4OrphanBlock, 4> &inode_bitmaps;
    const std::array<Ext4OrphanBlock, 4> &block_bitmaps;
    std::span<const Ext4OrphanInodeImage> chain{};
};

struct Ext4OrphanMember
{
    u32 number{}, generation{}, size{}, next{};
    std::array<u32, 16> data_blocks{};
    u32 data_count{};
};

// A model proof about supplied bytes, not a pathname decoder, mount admission,
// replay permission, reclaim proof or ownership proof against other inodes.
class Ext4OrphanChainProof final
{
  public:
    static constexpr usize capacity = 8;
    [[nodiscard]] usize count() const { return count_; }
    [[nodiscard]] const std::array<u8, 16> &filesystem_uuid() const { return filesystem_uuid_; }
    [[nodiscard]] u32 observed_head() const { return observed_head_; }

    [[nodiscard]] const Ext4OrphanMember *member(u32 number, u32 generation) const
    {
        for (usize index = 0; index < count_; ++index)
            if (members_[index].number == number && members_[index].generation == generation)
                return &members_[index];
        return nullptr;
    }
    [[nodiscard]] const Ext4OrphanMember *at(usize index) const
    { return index < count_ ? &members_[index] : nullptr; }

  private:
    friend class Ext4OrphanChainValidator;
    std::array<Ext4OrphanMember, capacity> members_{};
    usize count_{};
    std::array<u8, 16> filesystem_uuid_{};
    u32 observed_head_{};
};

class Ext4OrphanChainValidator final
{
    using Block = Ext4OrphanBlock;
    struct Geometry
    {
        u32 seed{};
        std::array<u32, 4> block_bitmap{}, inode_bitmap{}, table{};
        std::array<u16, 4> flags{}, block_checksum{}, inode_checksum{};
    };

    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 group, std::span<const u8> descriptor)
    {
        std::array<u8, 32> copy{};
        for (usize index = 0; index < copy.size(); ++index) copy[index] = descriptor[index];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), copy));
    }
    [[nodiscard]] static bool ordinary_block(const Ext4RecoveryInfo &binding, u32 block)
    {
        if (block < 3 || block >= binding.layout.filesystem_blocks) return false;
        if (block >= binding.layout.journal_first_block &&
            u64{block} - binding.layout.journal_first_block < binding.layout.journal_blocks) return false;
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto range = binding.metadata[index];
            if (block >= range.first && u64{block} - range.first < range.count) return false;
        }
        return true;
    }
    [[nodiscard]] static Status inspect(const Ext4RecoveryInfo &binding,
                                        const Ext4OrphanChainSource &source,
                                        Geometry &geometry)
    {
        using namespace ext4_checked;
        const auto s = std::span<const u8>{source.superblock};
        if (binding.layout.block_size != 1024 || binding.layout.filesystem_blocks != 32768 ||
            binding.journal_inode != 8 || binding.layout.journal_blocks < 1024 ||
            binding.metadata_count != 16 || !binding.structural_metadata().valid() ||
            binding.layout.journal_first_block < 3 ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks >
                binding.layout.filesystem_blocks - binding.layout.journal_first_block)
            return Status::unsupported("orphan chain bounded binding");
        if (jbd2::detail::crc32c(~u32{0}, s.first(1020)) != le32(s, 1020))
            return Status::io_error("orphan chain superblock checksum");
        if (le16(s, 56) != 0xef53 || le32(s, 0) != 8192 || le32(s, 4) != 32768 ||
            le32(s, 20) != 1 || le32(s, 24) || le32(s, 32) != 8192 ||
            le32(s, 40) != 2048 || le32(s, 84) < 11 || le16(s, 88) != 256 ||
            le32(s, 92) != 4 ||
            le32(s, 96) != (binding.needs_recovery ? 0x46U : 0x42U) ||
            le32(s, 100) != 0x400 || le32(s, 224) != 8 || s[373] != 1)
            return Status::unsupported("orphan chain superblock profile");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (s[104 + index] != binding.uuid[index])
                return Status::io_error("orphan chain UUID differs");
        geometry.seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        u32 free_inodes = 0;
        for (u32 group = 0; group < 4; ++group)
        {
            const auto d = std::span<const u8>{source.descriptors}.subspan(group * 32, 32);
            if (descriptor_checksum(geometry.seed, group, d) != le16(d, 30))
                return Status::io_error("orphan chain descriptor checksum");
            const u32 begin = 1 + group * 8192;
            const u32 blocks = group == 3 ? 8191 : 8192;
            const u32 block_bitmap = le32(d, 0), inode_bitmap = le32(d, 4), table = le32(d, 8);
            const std::array<jbd2::BlockRange, 4> expected{{
                {begin, 2}, {block_bitmap, 1}, {inode_bitmap, 1}, {table, 512}}};
            for (usize index = 0; index < expected.size(); ++index)
                if (binding.metadata[group * 4 + index].first != expected[index].first ||
                    binding.metadata[group * 4 + index].count != expected[index].count)
                    return Status::unsupported("orphan chain descriptor placement");
            geometry.block_bitmap[group] = block_bitmap;
            geometry.inode_bitmap[group] = inode_bitmap;
            geometry.table[group] = table;
            geometry.flags[group] = le16(d, 18);
            geometry.block_checksum[group] = le16(d, 24);
            geometry.inode_checksum[group] = le16(d, 26);
            if ((geometry.flags[group] & ~u16{7}) || le16(d, 12) > blocks ||
                le16(d, 14) > 2048 || le16(d, 16) > 2048 - le16(d, 14) ||
                le16(d, 28) > 2048 ||
                ((geometry.flags[group] & 1) &&
                 (le16(d, 14) != 2048 || le16(d, 16) || le16(d, 28) != 2048 ||
                  geometry.inode_checksum[group])) ||
                ((geometry.flags[group] & 2) &&
                 (le16(d, 12) != blocks - 516 || geometry.block_checksum[group])))
                return Status::io_error("orphan chain descriptor counters or flags");
            free_inodes += le16(d, 14);
        }
        if (free_inodes != le32(s, 16))
            return Status::io_error("orphan chain free inode counters differ");
        return {};
    }
    [[nodiscard]] static Status decode(const Ext4RecoveryInfo &binding,
                                       const Ext4OrphanChainSource &source,
                                       const Geometry &geometry,
                                       const Ext4OrphanInodeImage &image,
                                       Ext4OrphanMember &member)
    {
        using namespace ext4_checked;
        const u32 number = image.number;
        if (number < 11 || number > 8192)
            return Status::invalid_argument("orphan chain inode number");
        const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
        if (geometry.flags[group] & 1)
            return Status::unsupported("orphan chain inode group uninitialized");
        if (image.table_block != geometry.table[group] + local / 4)
            return Status::io_error("orphan chain table placement");
        const auto bitmap = std::span<const u8>{source.inode_bitmaps[group]}.first(256);
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != geometry.inode_checksum[group])
            return Status::io_error("orphan chain inode bitmap checksum");
        if (!(bitmap[local / 8] & (1U << (local % 8))))
            return Status::io_error("orphan chain inode is not allocated");
        const auto inode = std::span<const u8>{image.table}.subspan((local % 4) * 256, 256);
        if (auto status = inode_checksum(geometry.seed, number, inode); !status) return status;
        if (le32(inode, 100) != image.expected_generation)
            return Status::io_error("orphan chain inode generation differs");
        const u32 size = le32(inode, 4);
        if ((le16(inode, 0) & 0170000) != 0100000 || (le16(inode, 0) & 07000) ||
            le16(inode, 26) || size > 16384 || le32(inode, 32) != 0x80000 ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) > 4 ||
            le16(inode, 44) != 4 || le16(inode, 46) || le32(inode, 48) ||
            le32(inode, 104) || le32(inode, 108) || le32(inode, 112) ||
            le16(inode, 116) || le16(inode, 118) || le16(inode, 126) ||
            le32(inode, 156) || le32(inode, 8) > 0x7fffffffU ||
            le32(inode, 12) > 0x7fffffffU || le32(inode, 16) > 0x7fffffffU ||
            (le32(inode, 140) >> 2) >= 1000000000U ||
            (le32(inode, 132) >> 2) >= 1000000000U ||
            (le32(inode, 136) >> 2) >= 1000000000U)
            return Status::unsupported("orphan chain regular inode profile");
        for (usize at = 160; at < inode.size(); ++at)
            if (inode[at]) return Status::unsupported("orphan chain inode extra fields");
        const u32 expected_blocks = u32((u64{size} + 1023) / 1024);
        if (le32(inode, 28) != expected_blocks * 2 || (!size && le16(inode, 42)))
            return Status::unsupported("orphan chain inode sectors or empty extents");
        for (usize at = 52 + le16(inode, 42) * 12; at < 100; ++at)
            if (inode[at]) return Status::unsupported("orphan chain unused extent bytes");
        member = {number, image.expected_generation, size, le32(inode, 20), {}, 0};
        for (u32 record = 0; record < le16(inode, 42); ++record)
        {
            const usize at = 52 + record * 12;
            const u32 logical = le32(inode, at), length = le16(inode, at + 4);
            const u32 physical = le32(inode, at + 8);
            if (!length || length > expected_blocks - member.data_count ||
                logical != member.data_count || le16(inode, at + 6) ||
                !physical || physical > 32768 - length)
                return Status::unsupported("orphan chain requires initialized inline extents");
            for (u32 part = 0; part < length; ++part)
            {
                const u32 block = physical + part;
                if (!ordinary_block(binding, block))
                    return Status::unsupported("orphan chain data block is reserved");
                const u32 data_group = (block - 1) / 8192, local_block = (block - 1) % 8192;
                if (geometry.flags[data_group] & 2)
                    return Status::unsupported("orphan chain data group uninitialized");
                const auto data_bitmap = std::span<const u8>{source.block_bitmaps[data_group]};
                if (u16(jbd2::detail::crc32c(geometry.seed, data_bitmap)) !=
                    geometry.block_checksum[data_group])
                    return Status::io_error("orphan chain block bitmap checksum");
                if (!(data_bitmap[local_block / 8] & (1U << (local_block % 8))))
                    return Status::io_error("orphan chain data block is not allocated");
                for (u32 prior = 0; prior < member.data_count; ++prior)
                    if (member.data_blocks[prior] == block)
                        return Status::io_error("orphan chain extent aliases itself");
                member.data_blocks[member.data_count++] = block;
            }
        }
        if (member.data_count != expected_blocks)
            return Status::unsupported("orphan chain extent coverage");
        return {};
    }

  public:
    [[nodiscard]] static Status validate(const Ext4RecoveryInfo &binding,
                                         const Ext4OrphanChainSource &source,
                                         Ext4OrphanChainProof &output)
    {
        output = {};
        if (source.chain.size() > Ext4OrphanChainProof::capacity)
            return Status::unsupported("orphan chain exceeds bounded capacity");
        Geometry geometry{};
        if (auto status = inspect(binding, source, geometry); !status) return status;
        const u32 head = ext4_checked::le32(source.superblock, 232);
        if ((head == 0) != source.chain.empty())
            return Status::io_error("orphan chain head and supplied images differ");
        Ext4OrphanChainProof proposed{};
        proposed.filesystem_uuid_ = binding.uuid;
        proposed.observed_head_ = head;
        u32 cursor = head;
        for (usize index = 0; index < source.chain.size(); ++index)
        {
            const auto &image = source.chain[index];
            if (!cursor || image.number != cursor)
                return Status::io_error("orphan chain order or early terminator");
            for (usize prior = 0; prior < index; ++prior)
            {
                if (source.chain[prior].number == image.number)
                    return Status::io_error("orphan chain cycle");
                if (source.chain[prior].table_block == image.table_block &&
                    source.chain[prior].table != image.table)
                    return Status::io_error("orphan chain aliased table views differ");
            }
            if (auto status = decode(binding, source, geometry, image,
                                     proposed.members_[index]); !status) return status;
            for (usize earlier = 0; earlier < index; ++earlier)
                for (u32 one = 0; one < proposed.members_[earlier].data_count; ++one)
                    for (u32 two = 0; two < proposed.members_[index].data_count; ++two)
                        if (proposed.members_[earlier].data_blocks[one] ==
                            proposed.members_[index].data_blocks[two])
                            return Status::io_error("orphan chain members share data");
            cursor = proposed.members_[index].next;
            ++proposed.count_;
        }
        if (cursor)
            return Status::io_error("orphan chain unprovided successor or cycle");
        output = proposed;
        return {};
    }
};
} // namespace ok::fs
