#pragma once

#include "ok/fs/ext4_readonly.hpp"

namespace ok::fs
{

// A proposed single-block directory edit. This is not a journal transaction,
// mounted VFS operation, or authority to write either image to a device.
struct Ext4DirectoryRenamePlan
{
    u32 directory_inode{}, source_inode{}, directory_block{};
    u16 entry_offset{}; // Source dirent in the before image, including relocation.
    std::array<u8, 1024> before{}, after{};
};

class Ext4DirectoryRenamePlanner final
{
    using Inode = std::array<u8, 256>;
    using Block = std::array<u8, 1024>;

    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    {
        bytes[at] = u8(value);
        bytes[at + 1] = u8(value >> 8);
    }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    {
        bytes[at] = u8(value);
        bytes[at + 1] = u8(value >> 8);
        bytes[at + 2] = u8(value >> 16);
        bytes[at + 3] = u8(value >> 24);
    }
    [[nodiscard]] static bool valid_name(std::string_view name)
    {
        if (name.empty() || name.size() > 31 || name == "." || name == "..") return false;
        for (const char byte : name) if (!byte || byte == '/') return false;
        return true;
    }
    [[nodiscard]] static bool ordinary_block(const Ext4RecoveryInfo &binding, u32 block)
    {
        if (block < 2 || block >= binding.layout.filesystem_blocks) return false;
        if (block >= binding.layout.journal_first_block &&
            u64{block} - binding.layout.journal_first_block < binding.layout.journal_blocks) return false;
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto range = binding.metadata[index];
            if (block >= range.first && u64{block} - range.first < range.count) return false;
        }
        return true;
    }
    [[nodiscard]] static bool exact_preallocated_regular(const Ext4RecoveryInfo &binding,
                                                           std::span<const u8> inode, u32 directory_block)
    {
        using namespace ext4_checked;
        const u32 size = le32(inode, 4);
        if ((le16(inode, 0) & 0170000) != 0100000 || le16(inode, 26) != 1 ||
            !size || le32(inode, 32) != 0x80000 || le16(inode, 40) != 0xf30a ||
            !le16(inode, 42) || le16(inode, 42) > 4 || le16(inode, 44) != 4 || le16(inode, 46))
            return false;
        const u32 required = u32((u64{size} + 1023) / 1024);
        if (required > 16 || le32(inode, 28) != required * 2U) return false;
        u32 logical = 0;
        for (usize index = 0; index < le16(inode, 42); ++index)
        {
            const usize at = 52 + index * 12;
            const u32 start = le32(inode, at), length = le16(inode, at + 4);
            const u32 physical = le32(inode, at + 8);
            if (start != logical || !length || length > required - logical || length > 32768 ||
                le16(inode, at + 6) || physical > 0xffffffffU - length)
                return false;
            for (usize previous = 0; previous < index; ++previous)
            {
                const usize prior = 52 + previous * 12;
                const u32 prior_physical = le32(inode, prior + 8), prior_length = le16(inode, prior + 4);
                if (physical < u64{prior_physical} + prior_length &&
                    prior_physical < u64{physical} + length) return false;
            }
            for (u32 part = 0; part < length; ++part)
                if (physical + part == directory_block || !ordinary_block(binding, physical + part)) return false;
            logical += length;
        }
        return logical == required;
    }

    [[nodiscard]] static Status checked_source_directory(
        const Ext4RecoveryInfo &binding, u32 parent, u32 number, const Inode &inode,
        u32 parent_block, u32 physical, const Block &image, u32 seed)
    {
        using namespace ext4_checked;
        if (!ordinary_block(binding, physical) || physical == parent_block ||
            (le16(inode, 0) & 0170000) != 0040000 || le16(inode, 26) < 2 ||
            le32(inode, 20) || le32(inode, 104) || le32(inode, 108) || le32(inode, 112) ||
            le16(inode, 116) || le16(inode, 118) ||
            le32(inode, 4) != 1024 || le32(inode, 28) != 2 || le32(inode, 32) != 0x80000 ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) != 1 || le16(inode, 44) != 4 ||
            le16(inode, 46) || le32(inode, 52) || le16(inode, 56) != 1 ||
            le16(inode, 58) || le32(inode, 60) != physical)
            return Status::unsupported("bounded rename source directory extent");
        constexpr usize tail = 1012;
        if (le32(image, tail) || le16(image, tail + 4) != 12 || image[tail + 6] ||
            image[tail + 7] != 0xde || le32(image, tail + 8) != jbd2::detail::crc32c(
                inode_seed(seed, number, inode), std::span<const u8>{image}.first(tail)))
            return Status::io_error("bounded rename source directory checksum");
        usize dots{}, parents{}, child_directories{};
        for (usize offset = 0; offset < tail;)
        {
            if (tail - offset < 8) return Status::io_error("bounded rename source short dirent");
            const usize record = le16(image, offset + 4), length = image[offset + 6];
            const u32 entry = le32(image, offset);
            const u8 type = image[offset + 7];
            if (record < 8 || record % 4 || record > tail - offset || length > record - 8 ||
                (entry && (!length || !type || type > 7)))
                return Status::io_error("bounded rename source dirent bounds");
            if (entry)
            {
                const std::string_view name{reinterpret_cast<const char *>(image.data() + offset + 8), length};
                for (const char byte : name)
                    if (!byte || byte == '/') return Status::io_error("bounded rename source dirent name");
                for (usize prior = 0; prior < offset; prior += le16(image, prior + 4))
                    if (le32(image, prior) && name == std::string_view{
                            reinterpret_cast<const char *>(image.data() + prior + 8), image[prior + 6]})
                        return Status::io_error("bounded rename source duplicate name");
                if (name == ".")
                {
                    if (offset || record != 12 || entry != number || type != 2)
                        return Status::io_error("bounded rename source dot identity");
                    ++dots;
                }
                else if (name == "..")
                {
                    if (offset != 12 || entry != parent || type != 2)
                        return Status::io_error("bounded rename source parent identity");
                    ++parents;
                }
                else
                {
                    if (entry <= 10 || entry == parent || entry == number)
                        return Status::unsupported("bounded rename source child identity");
                    if (type == 2) ++child_directories;
                }
            }
            offset += record;
        }
        if (dots != 1 || parents != 1 || le16(inode, 26) != child_directories + 2)
            return Status::io_error("bounded rename source directory links");
        return {};
    }

    [[nodiscard]] static Result<Ext4DirectoryRenamePlan> plan_entry(
        const Ext4RecoveryInfo &binding, u32 directory_inode, const Inode &directory,
        u32 source_inode, const Inode &source, u32 directory_block,
        const Block &image, std::string_view old_name, std::string_view new_name,
        u32 source_block, const Block *source_image)
    {
        using namespace ext4_checked;
        if (!valid_name(old_name) || !valid_name(new_name) || old_name == new_name)
            return Status::invalid_argument("bounded rename component");
        if (binding.layout.block_size != 1024 || binding.metadata_count > binding.metadata.size() ||
            binding.layout.filesystem_blocks > 0xffffffffULL || directory_inode < 2 || source_inode <= 10 ||
            directory_inode == source_inode ||
            !ordinary_block(binding, directory_block))
            return Status::unsupported("bounded rename layout or target");
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        if (!inode_checksum(seed, directory_inode, directory) || !inode_checksum(seed, source_inode, source))
            return Status::io_error("bounded rename inode checksum");
        if ((le16(directory, 0) & 0170000) != 0040000 || le16(directory, 26) < 2 ||
            le32(directory, 4) != 1024 || le32(directory, 28) != 2 ||
            le32(directory, 32) != 0x80000 || le16(directory, 40) != 0xf30a ||
            le16(directory, 42) != 1 || le16(directory, 44) != 4 || le16(directory, 46) ||
            le32(directory, 52) || le16(directory, 56) != 1 ||
            le16(directory, 58) || le32(directory, 60) != directory_block)
            return Status::unsupported("bounded rename requires one checked parent directory block");
        const u8 source_type = source_image ? 2 : 1;
        if (source_image)
        {
            if (le32(directory, 20) || le32(directory, 104) || le32(directory, 108) ||
                le32(directory, 112) || le16(directory, 116) || le16(directory, 118))
                return Status::unsupported("bounded rename parent directory high or external metadata");
            if (le16(directory, 26) < 3)
                return Status::io_error("bounded rename parent lacks source directory link");
            if (const auto status = checked_source_directory(binding, directory_inode, source_inode,
                    source, directory_block, source_block, *source_image, seed); !status) return status;
        }
        else if (!exact_preallocated_regular(binding, source, directory_block))
            return Status::unsupported("bounded rename requires preallocated regular file");
        constexpr usize tail = 1024 - 12;
        if (le32(image, tail) || le16(image, tail + 4) != 12 || image[tail + 6] ||
            image[tail + 7] != 0xde ||
            jbd2::detail::crc32c(inode_seed(seed, directory_inode, directory),
                                 std::span<const u8>{image}.first(tail)) != le32(image, tail + 8))
            return Status::io_error("bounded rename directory tail checksum");

        usize source_offset = tail;
        usize source_name_length = 0;
        usize source_capacity = 0;
        usize source_links = 0;
        usize parent_dots = 0, parent_parents = 0;
        bool destination_found = false;
        for (usize offset = 0; offset < tail;)
        {
            if (tail - offset < 8) return Status::io_error("bounded rename short dirent");
            const usize record = le16(image, offset + 4);
            const usize length = image[offset + 6];
            const u32 number = le32(image, offset);
            const u8 type = image[offset + 7];
            if (record < 8 || record % 4 || record > tail - offset || length > record - 8 ||
                (number && (!length || type > 7)))
                return Status::io_error("bounded rename dirent bounds");
            const std::string_view name{reinterpret_cast<const char *>(image.data() + offset + 8), length};
            if (number)
            {
                if (number == source_inode) ++source_links;
                for (const char byte : name)
                    if (!byte || byte == '/') return Status::io_error("bounded rename invalid dirent name");
                if (source_image && name == ".")
                {
                    if (offset || record != 12 || number != directory_inode || type != 2)
                        return Status::io_error("bounded rename parent dot identity");
                    ++parent_dots;
                }
                if (source_image && name == "..")
                {
                    if (offset != 12 || number < 2 || number == source_inode || type != 2 ||
                        (directory_inode == 2 && number != 2))
                        return Status::io_error("bounded rename parent ancestor identity");
                    ++parent_parents;
                }
                if (name == old_name)
                {
                    if (source_offset != tail || number != source_inode || type != source_type)
                        return Status::io_error("bounded rename source dirent identity");
                    source_offset = offset; source_name_length = length; source_capacity = record - 8;
                }
                if (name == new_name) destination_found = true;
            }
            offset += record;
        }
        if (destination_found) return Status::already_exists("bounded rename destination exists");
        if (source_offset == tail) return Status::not_found("bounded rename source name absent");
        if (source_links != 1) return Status::io_error("bounded rename source has another directory alias");
        if (source_image && (parent_dots != 1 || parent_parents != 1))
            return Status::io_error("bounded rename parent dot entries");
        Ext4DirectoryRenamePlan result{};
        result.directory_inode = directory_inode;
        result.source_inode = source_inode;
        result.directory_block = directory_block;
        result.entry_offset = static_cast<u16>(source_offset);
        result.before = image; result.after = image;
        usize destination_offset = source_offset;
        if (new_name.size() > source_capacity)
        {
            // Only select space after every input record and the destination
            // absence have been checked. No new filesystem block is allocated.
            const usize required = (8 + new_name.size() + 3) & ~usize{3};
            usize free_offset = tail, slack_offset = tail, donor_minimum{};
            for (usize offset = 0; offset < tail;)
            {
                const usize record = le16(image, offset + 4);
                const usize length = image[offset + 6];
                const usize minimum = (8 + length + 3) & ~usize{3};
                if (!le32(image, offset))
                {
                    if (free_offset == tail && record >= required) free_offset = offset;
                }
                else if (offset != source_offset && slack_offset == tail && record - minimum >= required)
                {
                    slack_offset = offset; donor_minimum = minimum;
                }
                offset += record;
            }
            if (free_offset == tail && slack_offset == tail)
                return Status::no_space("bounded rename has no fitting directory record");
            usize destination_record{};
            if (free_offset != tail)
            {
                destination_offset = free_offset;
                destination_record = le16(image, free_offset + 4);
            }
            else
            {
                destination_offset = slack_offset + donor_minimum;
                destination_record = le16(image, slack_offset + 4) - donor_minimum;
                store16(result.after, slack_offset + 4, static_cast<u16>(donor_minimum));
            }
            const usize source_record = source_capacity + 8;
            for (usize index = 0; index < source_record; ++index) result.after[source_offset + index] = 0;
            store16(result.after, source_offset + 4, static_cast<u16>(source_record));
            for (usize index = 0; index < destination_record; ++index) result.after[destination_offset + index] = 0;
            store32(result.after, destination_offset, source_inode);
            store16(result.after, destination_offset + 4, static_cast<u16>(destination_record));
            result.after[destination_offset + 7] = image[source_offset + 7];
        }
        else
            for (usize index = 0; index < source_name_length; ++index)
                result.after[source_offset + 8 + index] = 0;
        result.after[destination_offset + 6] = static_cast<u8>(new_name.size());
        for (usize index = 0; index < new_name.size(); ++index)
            result.after[destination_offset + 8 + index] = static_cast<u8>(new_name[index]);
        store32(result.after, tail + 8, jbd2::detail::crc32c(
            inode_seed(seed, directory_inode, directory),
            std::span<const u8>{result.after}.first(tail)));
        return result;
    }

  public:
    [[nodiscard]] static Result<Ext4DirectoryRenamePlan> plan(
        const Ext4RecoveryInfo &binding, u32 directory_inode, const Inode &directory,
        u32 source_inode, const Inode &source, u32 directory_block,
        const Block &image, std::string_view old_name, std::string_view new_name)
    {
        return plan_entry(binding, directory_inode, directory, source_inode, source,
                          directory_block, image, old_name, new_name, 0, nullptr);
    }

    [[nodiscard]] static Result<Ext4DirectoryRenamePlan> plan_directory(
        const Ext4RecoveryInfo &binding, u32 directory_inode, const Inode &directory,
        u32 source_inode, const Inode &source, u32 directory_block, const Block &image,
        u32 source_block, const Block &source_image,
        std::string_view old_name, std::string_view new_name)
    {
        return plan_entry(binding, directory_inode, directory, source_inode, source,
                          directory_block, image, old_name, new_name, source_block, &source_image);
    }
};

} // namespace ok::fs
