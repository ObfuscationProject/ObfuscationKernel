#include "ok/fs/ext4_recovery.hpp"

namespace ok::fs
{
namespace
{
u16 le16(std::span<const u8> bytes, usize at)
{
    return u16{bytes[at]} | (u16{bytes[at + 1]} << 8);
}
u32 le32(std::span<const u8> bytes, usize at)
{
    return u32{le16(bytes, at)} | (u32{le16(bytes, at + 2)} << 16);
}
std::array<u8, 4> encoded(u32 value)
{
    return {u8(value), u8(value >> 8), u8(value >> 16), u8(value >> 24)};
}
bool overlap(u64 first, u64 count, u64 other, u64 length)
{
    // Inputs are already admitted, nonempty filesystem-relative ranges.
    return first < other + length && other < first + count;
}
bool allocated(std::span<const u8> bitmap, u32 index)
{
    return (bitmap[index / 8] & (u8{1} << (index % 8))) != 0;
}
Status corrupt(std::string_view message)
{
    return Status::io_error(message);
}
} // namespace

Status Ext4RecoveryBinding::inspect(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
{
    return inspect_impl(device, bounds, false, false);
}

Status Ext4RecoveryBinding::inspect_impl(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                         bool create_candidate, bool first_data_candidate)
{
    if (attempted_)
        return Status::already_exists("ext4 recovery binding is one-shot");
    attempted_ = true;
    create_candidate_ = create_candidate;
    first_data_candidate_ = first_data_candidate;
    device_ = &device;
    geometry_ = device.geometry();
    bounds_ = bounds;
    if (geometry_.block_size != 512)
        return Status::unsupported("ext4 recovery requires 512-byte sectors");
    if (bounds.first_sector >= geometry_.block_count || bounds.sector_count < 4 ||
        bounds.sector_count > geometry_.block_count - bounds.first_sector)
        return Status::invalid_argument("ext4 recovery device extent bounds");
    const auto status = admit();
    if (!status)
        return status;
    if (const auto unchanged = stable(); !unchanged)
        return unchanged;
    admitted_ = true;
    return Status::success();
}

Status Ext4RecoveryBinding::stable() const
{
    const auto current = device_->geometry();
    if (current.block_size != geometry_.block_size || current.block_count != geometry_.block_count ||
        current.writable != geometry_.writable)
        return Status::io_error("ext4 recovery device geometry changed");
    return Status::success();
}

Status Ext4RecoveryBinding::read_sectors(u64 relative, std::span<u8> out)
{
    if (out.empty() || out.size() % 512 || relative >= bounds_.sector_count ||
        out.size() / 512 > bounds_.sector_count - relative)
        return Status::invalid_argument("ext4 recovery read bounds");
    if (const auto unchanged = stable(); !unchanged)
        return unchanged;
    if (const auto status = device_->read_blocks(bounds_.first_sector + relative, std::as_writable_bytes(out)); !status)
        return status;
    return stable();
}

Status Ext4RecoveryBinding::read_block(u32 block)
{
    if (block >= info_.layout.filesystem_blocks)
        return corrupt("ext4 recovery block outside filesystem");
    return read_sectors(u64{block} * (info_.layout.block_size / 512),
                        std::span<u8>{block_}.first(info_.layout.block_size));
}

Status Ext4RecoveryBinding::add_range(u64 first, u64 count)
{
    if (!count || first >= info_.layout.filesystem_blocks || count > info_.layout.filesystem_blocks - first)
        return corrupt("ext4 recovery metadata bounds");
    for (usize index = 0; index < info_.metadata_count; ++index)
        if (overlap(first, count, info_.metadata[index].first, info_.metadata[index].count))
            return corrupt("ext4 recovery metadata overlap");
    if (info_.metadata_count == info_.metadata.size())
        return Status::unsupported("ext4 recovery metadata range capacity");
    info_.metadata[info_.metadata_count++] = {first, count};
    return Status::success();
}

Status Ext4RecoveryBinding::admit()
{
    if (const auto status = read_sectors(2, super_); !status)
        return status;
    const std::span<const u8> s{super_};
    if (le16(s, 56) != 0xef53 || le32(s, 24) > 2 || le32(s, 76) != 1 || le32(s, 72))
        return Status::unsupported("ext4 recovery superblock format");
    if (le32(s, 92) != 4 || (le32(s, 96) != 0x42 && le32(s, 96) != 0x46) ||
        le32(s, 100) != 0x400 || s[373] != 1 || le16(s, 88) != 256 || le16(s, 90) ||
        (le16(s, 254) != 0 && le16(s, 254) != 32) || le16(s, 206) ||
        le32(s, 336) || le32(s, 340) || le32(s, 344))
        return Status::unsupported("ext4 recovery feature or descriptor format");
    if (jbd2::detail::crc32c(~u32{0}, s.first(1020)) != le32(s, 1020))
        return corrupt("ext4 recovery superblock checksum");
    if (le16(s, 58) != 1 || le32(s, 232))
        return Status::unsupported("ext4 recovery error state or orphan chain");
    if (!jbd2::detail::all_zero(s.subspan(208, 16)) || le32(s, 224) != 8 || le32(s, 228))
        return Status::unsupported("ext4 recovery requires internal journal inode 8");
    info_.needs_recovery = (le32(s, 96) & 4) != 0;
    info_.layout.first_sector = bounds_.first_sector;
    info_.layout.block_size = 1024U << le32(s, 24);
    info_.layout.filesystem_blocks = le32(s, 4);
    inode_count_ = le32(s, 0);
    first_data_ = le32(s, 20);
    blocks_per_group_ = le32(s, 32);
    inodes_per_group_ = le32(s, 40);
    if (info_.layout.filesystem_blocks <= first_data_ || inode_count_ < 11 ||
        first_data_ != (info_.layout.block_size == 1024 ? 1U : 0U) ||
        !blocks_per_group_ || blocks_per_group_ > info_.layout.block_size * 8 || blocks_per_group_ % 8 ||
        !inodes_per_group_ || inodes_per_group_ > info_.layout.block_size * 8 || inodes_per_group_ % 8 ||
        inodes_per_group_ % (info_.layout.block_size / 256) ||
        le32(s, 28) != le32(s, 24) || le32(s, 36) != blocks_per_group_ ||
        le32(s, 84) < 11 || le32(s, 84) > inode_count_ ||
        info_.layout.filesystem_blocks > bounds_.sector_count / (info_.layout.block_size / 512) ||
        le32(s, 12) > info_.layout.filesystem_blocks - first_data_ || le32(s, 16) > inode_count_)
        return corrupt("ext4 recovery filesystem geometry");
    group_count_ = u32((info_.layout.filesystem_blocks - first_data_ - 1) / blocks_per_group_ + 1);
    if (group_count_ > groups_.size())
        return Status::unsupported("ext4 recovery group capacity");
    if (u64{group_count_} * inodes_per_group_ != inode_count_)
        return corrupt("ext4 recovery inode group geometry");
    for (usize index = 0; index < info_.uuid.size(); ++index)
        info_.uuid[index] = s[104 + index];
    seed_ = jbd2::detail::crc32c(~u32{0}, info_.uuid);
    if (const auto status = groups(); !status)
        return status;
    if (const auto status = journal(); !status)
        return status;
    return allocation();
}

Status Ext4RecoveryBinding::groups()
{
    if (const auto status = read_block(first_data_ + 1); !status)
        return status;
    for (usize index = 0; index < descriptors_.size(); ++index)
        descriptors_[index] = block_[index];
    const u32 table_blocks = u32(u64{inodes_per_group_} * 256 / info_.layout.block_size);
    for (u32 group = 0; group < group_count_; ++group)
    {
        auto d = std::span<u8>{descriptors_}.subspan(group * 32, 32);
        const auto expected = le16(d, 30);
        d[30] = d[31] = 0;
        const auto checksum = jbd2::detail::crc32c(jbd2::detail::crc32c(seed_, encoded(group)), d);
        d[30] = u8(expected);
        d[31] = u8(expected >> 8);
        if (u16(checksum) != expected)
            return corrupt("ext4 recovery group descriptor checksum");
        auto &g = groups_[group];
        g.begin = first_data_ + group * blocks_per_group_;
        g.end = u32(info_.layout.filesystem_blocks - g.begin < blocks_per_group_
                        ? info_.layout.filesystem_blocks : u64{g.begin} + blocks_per_group_);
        g.block_bitmap = le32(d, 0);
        g.inode_bitmap = le32(d, 4);
        g.table = le32(d, 8);
        g.table_blocks = table_blocks;
        g.block_checksum = le16(d, 24);
        g.inode_checksum = le16(d, 26);
        g.flags = le16(d, 18);
        if (g.end - g.begin < 2 || g.block_bitmap < g.begin || g.block_bitmap >= g.end ||
            g.inode_bitmap < g.begin || g.inode_bitmap >= g.end || g.table < g.begin ||
            g.table >= g.end || table_blocks > g.end - g.table ||
            le16(d, 12) > g.end - g.begin || le16(d, 14) > inodes_per_group_ ||
            le16(d, 16) > inodes_per_group_ - le16(d, 14) || le16(d, 28) > inodes_per_group_ ||
            (g.flags & ~7U) ||
            ((g.flags & 1) && (le16(d, 14) != inodes_per_group_ || le16(d, 16) ||
                               le16(d, 28) != inodes_per_group_ || g.inode_checksum)) ||
            ((g.flags & 2) && (g.end - g.begin < table_blocks + 4 ||
                               le16(d, 12) != g.end - g.begin - table_blocks - 4 || g.block_checksum)))
            return corrupt("ext4 recovery group placement, counts or initialization");
        for (const auto range : std::array<jbd2::BlockRange, 4>{{{g.begin, 2}, {g.block_bitmap, 1},
                                                               {g.inode_bitmap, 1}, {g.table, table_blocks}}})
            if (const auto status = add_range(range.first, range.count); !status)
                return status;
    }
    return Status::success();
}

Status Ext4RecoveryBinding::journal()
{
    constexpr u32 number = 8;
    const auto group = (number - 1) / inodes_per_group_;
    const auto offset = (number - 1) % inodes_per_group_ * 256;
    if (const auto status = read_block(groups_[group].table + offset / info_.layout.block_size); !status)
        return status;
    for (usize index = 0; index < journal_inode_.size(); ++index)
        journal_inode_[index] = block_[offset % info_.layout.block_size + index];
    const std::span<const u8> inode{journal_inode_};
    if (le16(inode, 128) != 32)
        return Status::unsupported("ext4 recovery journal inode extra size");
    const auto expected = u32{le16(inode, 124)} | (u32{le16(inode, 130)} << 16);
    journal_inode_[124] = journal_inode_[125] = journal_inode_[130] = journal_inode_[131] = 0;
    auto checksum = jbd2::detail::crc32c(seed_, encoded(number));
    checksum = jbd2::detail::crc32c(checksum, inode.subspan(100, 4));
    checksum = jbd2::detail::crc32c(checksum, inode);
    journal_inode_[124] = u8(expected);
    journal_inode_[125] = u8(expected >> 8);
    journal_inode_[130] = u8(expected >> 16);
    journal_inode_[131] = u8(expected >> 24);
    if (checksum != expected)
        return corrupt("ext4 recovery journal inode checksum");
    if (le16(inode, 0) != 0100600 || le16(inode, 2) || le16(inode, 24) || le16(inode, 26) != 1 ||
        le32(inode, 20) || le32(inode, 32) != 0x80000 || le32(inode, 104) || le32(inode, 108) ||
        le32(inode, 112) || le16(inode, 116) || le16(inode, 118) || le16(inode, 120) || le16(inode, 122))
        return Status::unsupported("ext4 recovery journal inode identity, size, flags or ACL");
    const u32 count = le16(inode, 56), first = le32(inode, 60);
    if (le16(inode, 40) != 0xf30a || le16(inode, 42) != 1 || le16(inode, 44) != 4 ||
        le16(inode, 46) || le32(inode, 52) || le16(inode, 58) || count < 1024 || count > 32768 ||
        le32(inode, 4) != count * info_.layout.block_size ||
        le32(inode, 28) != count * (info_.layout.block_size / 512))
        return Status::unsupported("ext4 recovery journal must be one initialized contiguous extent");
    if (first < first_data_ || first >= info_.layout.filesystem_blocks ||
        count > info_.layout.filesystem_blocks - first)
        return corrupt("ext4 recovery journal extent bounds");
    for (usize index = 0; index < info_.metadata_count; ++index)
        if (overlap(first, count, info_.metadata[index].first, info_.metadata[index].count))
            return corrupt("ext4 recovery journal overlaps structural metadata");
    info_.layout.journal_first_block = first;
    info_.layout.journal_blocks = count;
    info_.journal_inode = number;
    if (const auto status = read_block(first); !status)
        return status;
    const auto codec = jbd2::Codec::open(std::span<const u8>{block_}.first(info_.layout.block_size), count,
                                        info_.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
    if (!codec)
        return codec.status();
    const auto &sb = codec.value().superblock();
    if (sb.block_size != info_.layout.block_size || sb.max_blocks != count)
        return corrupt("ext4 recovery journal superblock differs from inode mapping");
    info_.journal_sequence = sb.sequence;
    info_.empty_log = sb.start_block == 0;
    if (!info_.needs_recovery && !info_.empty_log)
        return corrupt("ext4 recovery clean flag with a nonempty journal");
    return Status::success();
}

Status Ext4RecoveryBinding::allocation()
{
    for (u32 group = 0; group < group_count_; ++group)
    {
        const auto &g = groups_[group];
        const u64 journal_end = info_.layout.journal_first_block + info_.layout.journal_blocks;
        const u64 first = info_.layout.journal_first_block > g.begin ? info_.layout.journal_first_block : g.begin;
        const u64 end = journal_end < g.end ? journal_end : g.end;
        if (g.flags & 2)
        {
            // A checksummed BLOCK_UNINIT descriptor denotes only the known
            // reserved metadata; its physical bitmap bytes are not meaningful.
            if (first < end)
                return corrupt("ext4 recovery journal in an uninitialized block group");
        }
        else
        {
            if (const auto status = read_block(g.block_bitmap); !status)
                return status;
            const auto bitmap = std::span<const u8>{block_}.first(blocks_per_group_ / 8);
            if (u16(jbd2::detail::crc32c(seed_, bitmap)) != g.block_checksum)
            {
                // This exception is private to dirty published allocation
                // previews. The selected policy must prove every exact home
                // image (and directory witness where required) before writes.
                if (!first_data_candidate_ || !info_.needs_recovery || info_.empty_log ||
                    first_data_bitmap_mismatches_ != 0)
                    return corrupt("ext4 recovery block bitmap checksum");
                first_data_bitmap_mismatch_group_ = group;
                ++first_data_bitmap_mismatches_;
            }
            for (usize index = 0; index < info_.metadata_count; ++index)
            {
                const auto range = info_.metadata[index];
                if (range.first >= g.begin && range.first < g.end)
                    for (u64 block = range.first; block < range.first + range.count; ++block)
                        if (!allocated(bitmap, u32(block - g.begin)))
                            return corrupt("ext4 recovery structural metadata is not allocated");
            }
            for (u64 block = first; block < end; ++block)
                if (!allocated(bitmap, u32(block - g.begin)))
                    return corrupt("ext4 recovery journal block is not allocated");
        }
        if (g.flags & 1)
        {
            if (group == (info_.journal_inode - 1) / inodes_per_group_)
                return corrupt("ext4 recovery journal inode in an uninitialized inode group");
        }
        else
        {
            if (const auto status = read_block(g.inode_bitmap); !status)
                return status;
            const auto inode_bitmap = std::span<const u8>{block_}.first(inodes_per_group_ / 8);
            if (u16(jbd2::detail::crc32c(seed_, inode_bitmap)) != g.inode_checksum)
            {
                // This provisional exception is private to a no-write create
                // preview. The committed log must later prove both images.
                if (!create_candidate_ || !info_.needs_recovery || info_.empty_log ||
                    create_bitmap_mismatches_ != 0)
                    return corrupt("ext4 recovery inode bitmap checksum");
                create_bitmap_mismatch_group_ = group;
                ++create_bitmap_mismatches_;
            }
            if (group == (info_.journal_inode - 1) / inodes_per_group_ &&
                !allocated(inode_bitmap, (info_.journal_inode - 1) % inodes_per_group_))
                return corrupt("ext4 recovery journal inode is not allocated");
        }
    }
    return Status::success();
}

} // namespace ok::fs
