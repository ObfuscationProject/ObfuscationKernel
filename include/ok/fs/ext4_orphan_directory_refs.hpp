#pragma once

#include "ok/fs/ext4_orphan_ownership.hpp"

namespace ok::fs
{
// Prototype, pure-model input. Records are ordered by allocated directory
// inode number, then logical block number. Every mapped directory block in
// the checked inode tables must have exactly one caller-supplied record.
struct Ext4OrphanDirectoryBlockImage
{
    u32 inode{}, logical{}, physical{};
    Ext4OrphanBlock bytes{};
};

struct Ext4OrphanDirectoryReferenceSource
{
    Ext4OrphanOwnershipSource ownership;
    std::span<const Ext4OrphanDirectoryBlockImage> directory_blocks{};
};

struct Ext4OrphanDirectoryReferenceScratch
{
    struct Name
    {
        u32 directory{}, block_index{};
        u16 offset{};
        u8 length{};
    };
    static constexpr usize block_capacity = 256;
    static constexpr usize entry_capacity = 4096;
    Ext4OrphanOwnershipScratch ownership{};
    std::array<Name, entry_capacity> names{};
    usize name_count{};
};

struct Ext4OrphanDirectoryReferenceProof
{
    std::array<u8, 16> filesystem_uuid{};
    u32 observed_head{};
    usize directory_inodes{}, directory_blocks{}, live_entries{};
    bool valid{};
};

// This prototype proves that no parsed name in one complete supplied image
// points to an orphan-chain member. It does not prove image provenance, live
// descriptor/cache exclusion, mount coherence, or permission to free anything.
class Ext4OrphanDirectoryReferenceValidator final
{
    using Scratch = Ext4OrphanDirectoryReferenceScratch;

    [[nodiscard]] static bool inode_bit(const Ext4OrphanOwnershipSource &source, u32 number)
    {
        const u32 index = number - 1;
        const auto &bitmap = source.chain.inode_bitmaps[index / 2048];
        const u32 local = index % 2048;
        return (bitmap[local / 8] & (1U << (local % 8))) != 0;
    }

    [[nodiscard]] static std::span<const u8> inode_bytes(
        const Ext4OrphanOwnershipSource &source, u32 number)
    {
        const u32 index = number - 1, group = index / 2048, local = index % 2048;
        const auto &block = source.inode_tables[group][local / 4];
        return std::span<const u8>{block}.subspan((local % 4) * 256, 256);
    }

    [[nodiscard]] static u32 mapped(std::span<const u8> inode, u32 logical)
    {
        using namespace ext4_checked;
        for (u32 record = 0; record < le16(inode, 42); ++record)
        {
            const usize at = 52 + record * 12;
            const u32 start = le32(inode, at), length = le16(inode, at + 4);
            if (logical >= start && logical - start < length)
                return le32(inode, at + 8) + logical - start;
        }
        return 0;
    }

    [[nodiscard]] static bool orphan(const Ext4OrphanOwnershipSource &source, u32 child)
    {
        for (const auto &member : source.chain.chain)
            if (member.number == child) return true;
        return false;
    }

    [[nodiscard]] static bool name_is(std::span<const u8> bytes, usize offset,
                                      u8 length, const char *name, usize size)
    {
        if (length != size) return false;
        for (usize at = 0; at < size; ++at)
            if (bytes[offset + 8 + at] != u8(name[at])) return false;
        return true;
    }

    [[nodiscard]] static Status block_entries(
        const Ext4OrphanDirectoryReferenceSource &source,
        Ext4OrphanDirectoryReferenceScratch &scratch,
        u32 seed, u32 number, std::span<const u8> inode,
        usize block_index, u32 &dots, u32 &parents)
    {
        using namespace ext4_checked;
        const auto &image = source.directory_blocks[block_index];
        const auto bytes = std::span<const u8>{image.bytes};
        constexpr usize tail = 1024 - 12;
        if (le32(bytes, tail) || le16(bytes, tail + 4) != 12 || bytes[tail + 6] ||
            bytes[tail + 7] != 0xde ||
            jbd2::detail::crc32c(inode_seed(seed, number, inode), bytes.first(tail)) !=
                le32(bytes, tail + 8))
            return Status::io_error("orphan directory tail or checksum");

        for (usize offset = 0; offset < tail;)
        {
            if (tail - offset < 8)
                return Status::io_error("orphan directory short record");
            const u32 child = le32(bytes, offset);
            const u16 length = le16(bytes, offset + 4);
            const u8 name_length = bytes[offset + 6], type = bytes[offset + 7];
            if (length < 8 || (length & 3) || length > tail - offset ||
                name_length > length - 8 || child > 8192 ||
                (child && (!name_length || (type != 1 && type != 2 && type != 7))))
                return Status::io_error("orphan directory record bounds or type");
            if (child)
            {
                for (usize at = 0; at < name_length; ++at)
                    if (!bytes[offset + 8 + at] || bytes[offset + 8 + at] == '/')
                        return Status::io_error("orphan directory invalid name");
                if (orphan(source.ownership, child))
                    return Status::io_error("orphan directory name references orphan");
                if (!inode_bit(source.ownership, child))
                    return Status::io_error("orphan directory name references unallocated inode");
                const auto target = inode_bytes(source.ownership, child);
                const u16 target_type = le16(target, 0) & 0170000;
                if (type != (target_type == 0040000 ? 2U : target_type == 0100000 ? 1U :
                             target_type == 0120000 ? 7U : 0U))
                    return Status::io_error("orphan directory name and inode type differ");

                const bool dot = name_is(bytes, offset, name_length, ".", 1);
                const bool dotdot = name_is(bytes, offset, name_length, "..", 2);
                if (dot)
                {
                    if (child != number || type != 2 || ++dots != 1)
                        return Status::io_error("orphan directory invalid dot");
                }
                else if (dotdot)
                {
                    if (type != 2 || (number == 2 && child != 2) || ++parents != 1)
                        return Status::io_error("orphan directory invalid dotdot");
                }
                else if (child < 11)
                    return Status::unsupported("orphan directory names reserved inode");

                for (usize prior = 0; prior < scratch.name_count; ++prior)
                {
                    const auto &name = scratch.names[prior];
                    if (name.directory != number || name.length != name_length) continue;
                    const auto previous = std::span<const u8>{
                        source.directory_blocks[name.block_index].bytes};
                    bool same = true;
                    for (usize at = 0; at < name_length; ++at)
                        if (previous[name.offset + 8 + at] != bytes[offset + 8 + at])
                        { same = false; break; }
                    if (same) return Status::io_error("orphan directory duplicate name");
                }
                if (scratch.name_count == scratch.names.size())
                    return Status::unsupported("orphan directory entry budget");
                scratch.names[scratch.name_count++] = {
                    number, u32(block_index), u16(offset), name_length};
            }
            offset += length;
        }
        return {};
    }

  public:
    [[nodiscard]] static Status validate(const Ext4RecoveryInfo &binding,
                                         const Ext4OrphanDirectoryReferenceSource &source,
                                         Ext4OrphanDirectoryReferenceScratch &scratch,
                                         Ext4OrphanDirectoryReferenceProof &output)
    {
        using namespace ext4_checked;
        output = {}; scratch = {};
        if (source.directory_blocks.size() > Scratch::block_capacity)
            return Status::unsupported("orphan directory block budget");
        Ext4OrphanOwnershipProof ownership{};
        if (auto status = Ext4OrphanOwnershipValidator::validate(
                binding, source.ownership, scratch.ownership, ownership); !status)
            return status;
        if (!ownership.valid)
            return Status::fault("orphan directory ownership proof absent");
        for (usize one = 0; one < source.directory_blocks.size(); ++one)
            for (usize earlier = 0; earlier < one; ++earlier)
                if ((source.directory_blocks[one].inode == source.directory_blocks[earlier].inode &&
                     source.directory_blocks[one].logical == source.directory_blocks[earlier].logical) ||
                    source.directory_blocks[one].physical == source.directory_blocks[earlier].physical)
                    return Status::io_error("orphan directory duplicate block image");

        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        usize cursor = 0, directory_inodes = 0;
        for (u32 number = 1; number <= 8192; ++number)
        {
            if (!inode_bit(source.ownership, number)) continue;
            const auto inode = inode_bytes(source.ownership, number);
            if ((le16(inode, 0) & 0170000) != 0040000)
            {
                if (number == 2)
                    return Status::unsupported("orphan directory root inode is not a directory");
                continue;
            }
            if (number != 2 && number < 11)
                return Status::unsupported("orphan directory reserved inode profile");
            const u32 size = le32(inode, 4);
            if (!size || size > 64 * 1024 || size % 1024)
                return Status::unsupported("orphan directory size profile");
            ++directory_inodes;
            u32 dots = 0, parents = 0;
            for (u32 logical = 0; logical < size / 1024; ++logical)
            {
                if (cursor == Scratch::block_capacity)
                    return Status::unsupported("orphan directory block budget");
                if (cursor == source.directory_blocks.size())
                    return Status::invalid_argument("orphan directory block image missing");
                const u32 physical = mapped(inode, logical);
                const auto &image = source.directory_blocks[cursor];
                if (!physical || image.inode != number || image.logical != logical ||
                    image.physical != physical)
                    return Status::io_error("orphan directory block mapping differs");
                if (auto status = block_entries(source, scratch, seed, number, inode,
                                                cursor, dots, parents); !status)
                    return status;
                ++cursor;
            }
            if (dots != 1 || parents != 1)
                return Status::io_error("orphan directory dot entries missing");
        }
        if (cursor != source.directory_blocks.size())
            return Status::invalid_argument("orphan directory unexpected extra block image");
        output.filesystem_uuid = ownership.filesystem_uuid;
        output.observed_head = ownership.observed_head;
        output.directory_inodes = directory_inodes;
        output.directory_blocks = cursor;
        output.live_entries = scratch.name_count;
        output.valid = true;
        return {};
    }
};
} // namespace ok::fs
