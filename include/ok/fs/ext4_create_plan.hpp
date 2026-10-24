#pragma once

#include "ok/fs/ext4_readonly.hpp"
#include "ok/fs/ext4_free_inode.hpp"
#include "ok/fs/ext4_directory_create_sdk_profile.hpp"

namespace ok::fs
{
using Ext4CreateBlock = std::array<u8, 1024>;

struct Ext4CreateUpdate
{
    u32 parent{}, seconds{}, nanoseconds{};
    std::string_view name{};
    InitialNodeAttributes attributes{};
};

// The owner obtains these six old images from one qualified, exclusive view.
// Parent and new inode tables may name the same physical block, in which case
// their images must be identical. No disk access is performed by this planner.
struct Ext4CreateSource
{
    u32 inode{}, group{}, parent_table_block{}, new_table_block{}, directory_block{};
    const Ext4CreateBlock &superblock;
    const Ext4CreateBlock &descriptors;
    const Ext4CreateBlock &inode_bitmap;
    const Ext4CreateBlock &parent_table;
    const Ext4CreateBlock &new_table;
    const Ext4CreateBlock &directory;
};

struct Ext4CreatePlan
{
    u32 inode{}, parent{}, directory_block{}, allocated_directory_block{}, block_group{};
    // The directory successor adds a block bitmap and one initialized data
    // block. Empty regular-file plans retain their original five/six homes.
    std::array<u32, ext4_directory_create_home_capacity> targets{};
    std::array<Ext4CreateBlock, ext4_directory_create_home_capacity> before{}, after{};
    usize count{};
};

// Bounded 1 KiB empty regular-file allocation. This is an image planner, not
// journal authority, a mounted namespace operation, or a generic allocator.
class Ext4CreatePlanner final
{
    using Block = Ext4CreateBlock;
    struct Geometry
    {
        u32 groups{}, inodes{}, inodes_per_group{}, first_inode{}, table_blocks{}, seed{};
    };
    struct Group
    {
        u32 bitmap{}, table{};
        u16 free_inodes{}, unused_inodes{}, flags{};
    };

    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    {
        bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8);
    }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    {
        for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8));
    }
    [[nodiscard]] static constexpr usize minimum_record(usize name_length)
    { return (8 + name_length + 3) & ~usize{3}; }
    [[nodiscard]] static bool valid_name(std::string_view name)
    {
        if (name.empty() || name.size() > 31 || name == "." || name == "..") return false;
        for (char byte : name) if (!byte || byte == '/') return false;
        return true;
    }
    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 number, std::span<const u8> bytes)
    {
        std::array<u8, 32> copy{};
        for (usize index = 0; index < copy.size(); ++index) copy[index] = bytes[index];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, ext4_checked::encoded(number)), copy));
    }
    static void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
    {
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(crc)); store16(inode, 130, u16(crc >> 16));
    }
    [[nodiscard]] static Status inspect(const Ext4RecoveryInfo &binding, const Block &superblock,
                                        const Block &descriptors, Geometry &geometry)
    {
        using namespace ext4_checked;
        const auto s = std::span<const u8>{superblock};
        if (binding.layout.block_size != 1024 || binding.layout.filesystem_blocks > 0xffffffffULL ||
            binding.layout.filesystem_blocks < 3 || binding.journal_inode != 8 ||
            binding.layout.journal_blocks < 1024 ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks >
                binding.layout.filesystem_blocks - binding.layout.journal_first_block ||
            binding.metadata_count > binding.metadata.size() ||
            !binding.metadata_count || binding.metadata_count % 4 ||
            !binding.structural_metadata().valid())
            return Status::unsupported("bounded create binding layout");
        for (usize index = 0; index < binding.metadata_count; ++index)
        {
            const auto one = binding.metadata[index];
            if (one.first < binding.layout.journal_first_block + binding.layout.journal_blocks &&
                binding.layout.journal_first_block < one.first + one.count)
                return Status::io_error("bounded create metadata aliases journal");
            for (usize previous = 0; previous < index; ++previous)
            {
                const auto other = binding.metadata[previous];
                if (one.first < other.first + other.count && other.first < one.first + one.count)
                    return Status::io_error("bounded create metadata aliases metadata");
            }
        }
        if (jbd2::detail::crc32c(~u32{0}, s.first(1020)) != le32(s, 1020))
            return Status::io_error("bounded create superblock checksum");
        if (le16(s, 56) != 0xef53 || le32(s, 24) || le32(s, 20) != 1 ||
            le32(s, 4) != binding.layout.filesystem_blocks || le32(s, 76) != 1 || le32(s, 72) ||
            le32(s, 92) != 4 || le32(s, 96) != (binding.needs_recovery ? 0x46U : 0x42U) ||
            le32(s, 100) != 0x400 || le16(s, 88) != 256 || le16(s, 90) || le16(s, 58) != 1 ||
            le16(s, 206) || le32(s, 224) != 8 || le32(s, 228) || le32(s, 232) ||
            s[373] != 1 || (le16(s, 254) != 0 && le16(s, 254) != 32) ||
            le32(s, 336) || le32(s, 340) || le32(s, 344) ||
            !jbd2::detail::all_zero(s.subspan(208, 16)))
            return Status::unsupported("bounded create superblock profile");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (s[104 + index] != binding.uuid[index])
                return Status::io_error("bounded create filesystem UUID");
        const u32 blocks_per_group = le32(s, 32);
        geometry.inodes = le32(s, 0);
        geometry.inodes_per_group = le32(s, 40);
        geometry.first_inode = le32(s, 84);
        if (!blocks_per_group || blocks_per_group > 8192 || blocks_per_group % 8 ||
            !geometry.inodes_per_group || geometry.inodes_per_group > 8192 ||
            geometry.inodes_per_group % 8 || geometry.inodes_per_group % 4 ||
            le32(s, 28) != 0 || le32(s, 36) != blocks_per_group ||
            geometry.first_inode < 11 || geometry.first_inode > geometry.inodes ||
            le32(s, 12) > binding.layout.filesystem_blocks - 1 ||
            le32(s, 16) > geometry.inodes)
            return Status::io_error("bounded create group geometry");
        geometry.groups = u32((binding.layout.filesystem_blocks - 2) / blocks_per_group + 1);
        geometry.table_blocks = geometry.inodes_per_group / 4;
        geometry.seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        if (geometry.groups > Ext4RecoveryInfo::group_capacity ||
            geometry.groups * 4 != binding.metadata_count ||
            u64{geometry.groups} * geometry.inodes_per_group != geometry.inodes)
            return Status::unsupported("bounded create group capacity");
        u32 free_total = 0;
        for (u32 number = 0; number < geometry.groups; ++number)
        {
            const auto d = std::span<const u8>{descriptors}.subspan(number * 32, 32);
            if (descriptor_checksum(geometry.seed, number, d) != le16(d, 30))
                return Status::io_error("bounded create descriptor checksum");
            const u32 begin = 1 + number * blocks_per_group;
            const u32 count = u32(binding.layout.filesystem_blocks - begin < blocks_per_group
                ? binding.layout.filesystem_blocks - begin : blocks_per_group);
            const u32 bitmap = le32(d, 4), table = le32(d, 8);
            const std::array<jbd2::BlockRange, 4> expected{{{begin, 2}, {le32(d, 0), 1},
                                                              {bitmap, 1}, {table, geometry.table_blocks}}};
            for (usize index = 0; index < expected.size(); ++index)
                if (binding.metadata[number * 4 + index].first != expected[index].first ||
                    binding.metadata[number * 4 + index].count != expected[index].count)
                    return Status::unsupported("bounded create group placement changed");
            const u16 flags = le16(d, 18);
            if (count < 2 || (flags & ~u16{7}) || le16(d, 12) > count ||
                le16(d, 14) > geometry.inodes_per_group ||
                le16(d, 16) > geometry.inodes_per_group - le16(d, 14) ||
                le16(d, 28) > le16(d, 14) ||
                ((flags & 1) && (le16(d, 14) != geometry.inodes_per_group ||
                                 le16(d, 16) || le16(d, 28) != geometry.inodes_per_group || le16(d, 26))) ||
                ((flags & 2) && (count < geometry.table_blocks + 4 ||
                                 le16(d, 12) != count - geometry.table_blocks - 4 || le16(d, 24))))
                return Status::io_error("bounded create group counters or flags");
            free_total += le16(d, 14);
        }
        if (free_total != le32(s, 16))
            return Status::io_error("bounded create free inode counters disagree");
        return {};
    }
    [[nodiscard]] static Status group(const Block &descriptors, u32 number, const Geometry &geometry,
                                      Group &result)
    {
        const auto d = std::span<const u8>{descriptors}.subspan(number * 32, 32);
        result = {ext4_checked::le32(d, 4), ext4_checked::le32(d, 8),
                  ext4_checked::le16(d, 14), ext4_checked::le16(d, 28), ext4_checked::le16(d, 18)};
        if (result.flags & 1)
            return Status::unsupported("bounded create uninitialized inode bitmap");
        if (result.unused_inodes > result.free_inodes || result.free_inodes > geometry.inodes_per_group)
            return Status::io_error("bounded create inode group counters");
        return {};
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
    [[nodiscard]] static Status append(Ext4CreatePlan &output, u32 block, const Block &before, usize &index)
    {
        for (index = 0; index < output.count; ++index)
            if (output.targets[index] == block)
                return output.before[index] == before ? Status{} :
                    Status::io_error("bounded create aliased block images disagree");
        if (output.count == output.targets.size())
            return Status::no_memory("bounded create participant capacity");
        index = output.count++;
        output.targets[index] = block;
        output.before[index] = before;
        output.after[index] = before;
        return {};
    }

  public:
    [[nodiscard]] static Result<u32> select_inode(const Ext4RecoveryInfo &binding,
                                                  const Block &superblock, const Block &descriptors,
                                                  const Block &inode_bitmap, u32 group_number)
    {
        using namespace ext4_checked;
        Geometry geometry{};
        if (auto status = inspect(binding, superblock, descriptors, geometry); !status) return status;
        if (group_number >= geometry.groups)
            return Status::invalid_argument("bounded create inode group number");
        Group selected{};
        if (auto status = group(descriptors, group_number, geometry, selected); !status) return status;
        const auto bitmap = std::span<const u8>{inode_bitmap}.first(geometry.inodes_per_group / 8);
        const auto d = std::span<const u8>{descriptors}.subspan(group_number * 32, 32);
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != le16(d, 26))
            return Status::io_error("bounded create inode bitmap checksum");
        u32 free_count = 0, candidate = 0;
        const u32 unused_start = geometry.inodes_per_group - selected.unused_inodes;
        for (u32 local = 0; local < geometry.inodes_per_group; ++local)
        {
            const bool allocated = (bitmap[local / 8] & (1U << (local % 8))) != 0;
            const u32 number = group_number * geometry.inodes_per_group + local + 1;
            if (number < geometry.first_inode && !allocated)
                return Status::io_error("bounded create reserved inode bit is clear");
            if (local >= unused_start && allocated)
                return Status::io_error("bounded create unused inode suffix is allocated");
            if (!allocated)
            {
                ++free_count;
                if (!candidate && number >= geometry.first_inode) candidate = number;
            }
        }
        if (free_count != selected.free_inodes)
            return Status::io_error("bounded create inode bitmap count differs from descriptor");
        if (!candidate) return Status::no_space("bounded create no ordinary inode in group");
        return candidate;
    }

    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding, const Ext4CreateUpdate &update,
                                     const Ext4CreateSource &source, Ext4CreatePlan &output)
    {
        using namespace ext4_checked;
        output.count = 0;
        if (update.attributes.type != NodeType::regular)
            return Status::unsupported("bounded create requires a regular file");
        if (!valid_name(update.name) || (update.attributes.mode & ~u32{0777}) ||
            update.seconds > 0x7fffffffU ||
            update.nanoseconds >= 1000000000U)
            return Status::invalid_argument("bounded create name, regular mode or timestamp");
        Geometry geometry{};
        if (auto status = inspect(binding, source.superblock, source.descriptors, geometry); !status) return status;
        const auto chosen = select_inode(binding, source.superblock, source.descriptors,
                                         source.inode_bitmap, source.group);
        if (!chosen) return chosen.status();
        if (source.inode != chosen.value() || update.parent < 2 || update.parent > geometry.inodes ||
            update.parent == source.inode)
            return Status::invalid_argument("bounded create inode identity");
        Group selected{};
        if (auto status = group(source.descriptors, source.group, geometry, selected); !status) return status;
        const u32 parent_group = (update.parent - 1) / geometry.inodes_per_group;
        const u32 parent_local = (update.parent - 1) % geometry.inodes_per_group;
        const u32 new_local = (source.inode - 1) % geometry.inodes_per_group;
        const u32 parent_table = le32(std::span<const u8>{source.descriptors}.subspan(parent_group * 32, 32), 8) +
                                 parent_local / 4;
        const u32 new_table = selected.table + new_local / 4;
        if (source.parent_table_block != parent_table || source.new_table_block != new_table ||
            !ordinary_block(binding, source.directory_block) ||
            (parent_table == new_table && source.parent_table != source.new_table))
            return Status::unsupported("bounded create participant placement or aliased table images");
        const usize parent_offset = (parent_local % 4) * 256;
        const usize new_offset = (new_local % 4) * 256;
        const auto parent_inode = std::span<const u8>{source.parent_table}.subspan(parent_offset, 256);
        const auto empty_inode = std::span<const u8>{source.new_table}.subspan(new_offset, 256);
        if (auto status = inode_checksum(geometry.seed, update.parent, parent_inode); !status) return status;
        if ((le16(parent_inode, 0) & 0170000) != 0040000 || le16(parent_inode, 26) < 2 ||
            le32(parent_inode, 4) != 1024 || le32(parent_inode, 28) != 2 ||
            le32(parent_inode, 32) != 0x80000 || le16(parent_inode, 40) != 0xf30a ||
            le16(parent_inode, 42) != 1 || le16(parent_inode, 44) != 4 ||
            le16(parent_inode, 46) || le32(parent_inode, 52) || le16(parent_inode, 56) != 1 ||
            le16(parent_inode, 58) || le32(parent_inode, 60) != source.directory_block ||
            (le16(parent_inode, 0) & 0300) != 0300)
            return Status::unsupported("bounded create parent directory profile");
        const auto generation = Ext4FreeInode::inspect(empty_inode, source.inode, geometry.seed);
        if (!generation) return generation.status();
        const u64 version = u64{le32(parent_inode, 36)} | (u64{le32(parent_inode, 152)} << 32);
        if (version == ~u64{0}) return Status::overflow("bounded create parent version exhausted");
        constexpr usize tail = 1012;
        const auto directory = std::span<const u8>{source.directory};
        if (le32(directory, tail) || le16(directory, tail + 4) != 12 || directory[tail + 6] ||
            directory[tail + 7] != 0xde ||
            jbd2::detail::crc32c(inode_seed(geometry.seed, update.parent, parent_inode),
                                 directory.first(tail)) != le32(directory, tail + 8))
            return Status::io_error("bounded create directory tail checksum");
        usize slot = tail, split = 0;
        const usize needed = minimum_record(update.name.size());
        for (usize offset = 0; offset < tail;)
        {
            if (tail - offset < 8) return Status::io_error("bounded create short directory record");
            const u32 number = le32(directory, offset);
            const usize record = le16(directory, offset + 4), length = directory[offset + 6];
            const u8 type = directory[offset + 7];
            if (record < 8 || record % 4 || record > tail - offset || length > record - 8 ||
                number > geometry.inodes || (number && (!length || type > 7)))
                return Status::io_error("bounded create dirent bounds");
            if (number)
            {
                const std::string_view name{reinterpret_cast<const char *>(directory.data() + offset + 8), length};
                for (char byte : name)
                    if (!byte || byte == '/') return Status::io_error("bounded create invalid dirent name");
                if (name == update.name) return Status::already_exists("bounded create name exists");
                if (number == source.inode)
                    return Status::io_error("bounded create free inode already named");
                const usize minimum = minimum_record(length);
                if (slot == tail && record - minimum >= needed)
                {
                    // A zero predecessor lets ordered replay invert an already
                    // checkpointed directory insertion exactly.
                    bool zero_slack = true;
                    for (usize index = offset + minimum; index < offset + record; ++index)
                        if (directory[index]) { zero_slack = false; break; }
                    if (zero_slack) { slot = offset + minimum; split = minimum; }
                }
            }
            else if (slot == tail && record >= needed && !length && !type)
            {
                // Namespace unlink leaves one canonical empty record. The
                // complete create witness retains its original record length
                // and bytes; no inverse-directory guess supplies that history.
                bool empty_record = true;
                for (usize index = offset; index < offset + record; ++index)
                    if (index != offset + 4 && index != offset + 5 && directory[index])
                    { empty_record = false; break; }
                if (empty_record) { slot = offset; split = 0; }
            }
            offset += record;
        }
        if (slot == tail) return Status::no_space("bounded create directory block has no slot");
        usize parent_index{}, new_index{}, bitmap_index{}, descriptor_index{}, super_index{}, directory_index{};
        if (auto status = append(output, parent_table, source.parent_table, parent_index); !status) return status;
        if (auto status = append(output, new_table, source.new_table, new_index); !status) return status;
        if (auto status = append(output, selected.bitmap, source.inode_bitmap, bitmap_index); !status) return status;
        if (auto status = append(output, 2, source.descriptors, descriptor_index); !status) return status;
        if (auto status = append(output, 1, source.superblock, super_index); !status) return status;
        if (auto status = append(output, source.directory_block, source.directory, directory_index); !status)
            return status;
        if (output.count < 5 || output.count > 6 ||
            bitmap_index == parent_index || bitmap_index == new_index ||
            descriptor_index == parent_index || descriptor_index == new_index ||
            super_index == parent_index || super_index == new_index ||
            directory_index != output.count - 1)
            return Status::unsupported("bounded create prohibited participant alias");
        auto parent_after = std::span<u8>{output.after[parent_index]}.subspan(parent_offset, 256);
        store32(parent_after, 12, update.seconds); store32(parent_after, 132, update.nanoseconds << 2);
        store32(parent_after, 16, update.seconds); store32(parent_after, 136, update.nanoseconds << 2);
        store32(parent_after, 36, u32(version + 1)); store32(parent_after, 152, u32((version + 1) >> 32));
        stamp_inode(parent_after, update.parent, geometry.seed);
        auto new_after = std::span<u8>{output.after[new_index]}.subspan(new_offset, 256);
        for (u8 &byte : new_after) byte = 0;
        store32(new_after, 100, generation.value());
        store16(new_after, 0, u16(0100000 | update.attributes.mode));
        store16(new_after, 2, u16(update.attributes.uid));
        store16(new_after, 120, u16(update.attributes.uid >> 16));
        store16(new_after, 24, u16(update.attributes.gid));
        store16(new_after, 122, u16(update.attributes.gid >> 16));
        store16(new_after, 26, 1);
        store32(new_after, 8, update.seconds); store32(new_after, 140, update.nanoseconds << 2);
        store32(new_after, 12, update.seconds); store32(new_after, 132, update.nanoseconds << 2);
        store32(new_after, 16, update.seconds); store32(new_after, 136, update.nanoseconds << 2);
        store32(new_after, 144, update.seconds); store32(new_after, 148, update.nanoseconds << 2);
        store32(new_after, 32, 0x80000);
        store16(new_after, 40, 0xf30a); store16(new_after, 44, 4);
        store16(new_after, 128, 32);
        stamp_inode(new_after, source.inode, geometry.seed);
        auto &bitmap_after = output.after[bitmap_index];
        bitmap_after[new_local / 8] |= u8(1U << (new_local % 8));
        auto descriptor_after = std::span<u8>{output.after[descriptor_index]}.subspan(source.group * 32, 32);
        store16(descriptor_after, 14, u16(selected.free_inodes - 1));
        const u32 unused_start = geometry.inodes_per_group - selected.unused_inodes;
        if (new_local >= unused_start)
            store16(descriptor_after, 28, u16(geometry.inodes_per_group - new_local - 1));
        store16(descriptor_after, 26, u16(jbd2::detail::crc32c(
            geometry.seed, std::span<const u8>{bitmap_after}.first(geometry.inodes_per_group / 8))));
        store16(descriptor_after, 30, 0);
        store16(descriptor_after, 30, descriptor_checksum(geometry.seed, source.group, descriptor_after));
        auto &super_after = output.after[super_index];
        store32(super_after, 16, le32(source.superblock, 16) - 1);
        store32(super_after, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super_after}.first(1020)));
        auto &directory_after = output.after[directory_index];
        if (split)
        {
            const usize previous = slot - split;
            const u16 old_record = le16(source.directory, previous + 4);
            store16(directory_after, previous + 4, u16(split));
            store16(directory_after, slot + 4, u16(old_record - split));
        }
        store32(directory_after, slot, source.inode);
        if (!split) store16(directory_after, slot + 4, le16(source.directory, slot + 4));
        directory_after[slot + 6] = u8(update.name.size());
        directory_after[slot + 7] = 1;
        for (usize index = 0; index < update.name.size(); ++index)
            directory_after[slot + 8 + index] = u8(update.name[index]);
        store32(directory_after, tail + 8, jbd2::detail::crc32c(
            inode_seed(geometry.seed, update.parent, parent_after),
            std::span<const u8>{directory_after}.first(tail)));
        output.inode = source.inode; output.parent = update.parent; output.directory_block = source.directory_block;
        output.allocated_directory_block = output.block_group = 0;
        return {};
    }
};
} // namespace ok::fs
