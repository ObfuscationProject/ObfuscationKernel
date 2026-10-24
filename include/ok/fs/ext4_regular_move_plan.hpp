#pragma once

#include "ok/fs/ext4_readonly.hpp"

namespace ok::fs
{
using Ext4RegularMoveBlock = std::array<u8, 1024>;

struct Ext4RegularMoveUpdate
{
    u32 from_parent{}, to_parent{}, source{};
    u32 from_generation{}, to_generation{}, source_generation{};
    std::string_view old_name{}, new_name{};
    u32 seconds{}, nanoseconds{};
};

// Conservative ancillary admission, with no authority to create an orphan.
struct Ext4RegularMoveEmptyOrphanAuthorization
{
    std::array<u8, 16> filesystem_uuid{};
    u32 observed_head{};
    bool independently_checked_empty{};
};

// One exclusive stable old view. Allocation and orphan images remain read-only.
struct Ext4RegularMoveSource
{
    u32 from_parent_table_block{}, to_parent_table_block{}, source_table_block{};
    u32 from_directory_block{}, to_directory_block{};
    const Ext4RegularMoveBlock &superblock;
    const Ext4RegularMoveBlock &descriptors;
    const Ext4RegularMoveBlock &from_parent_table;
    const Ext4RegularMoveBlock &to_parent_table;
    const Ext4RegularMoveBlock &source_table;
    const Ext4RegularMoveBlock &from_directory;
    const Ext4RegularMoveBlock &to_directory;
    const std::array<Ext4RegularMoveBlock, 4> &inode_bitmaps;
    const std::array<Ext4RegularMoveBlock, 4> &block_bitmaps;
    const Ext4RegularMoveEmptyOrphanAuthorization &orphan;
};

struct Ext4RegularMovePlan
{
    static constexpr usize capacity = 5; // Two directories and three coalesced inode tables.
    std::array<u32, capacity> targets{};
    std::array<Ext4RegularMoveBlock, capacity> before{}, after{};
    std::array<u32, 16> retained_data_blocks{};
    std::array<char, 31> old_name{}, new_name{};
    usize count{}, retained_data_count{}, old_name_length{}, new_name_length{};
    u32 from_parent{}, to_parent{}, source{}, source_generation{};
    u16 source_entry_offset{}, destination_entry_offset{};
    bool allocation_unchanged{}, file_identity_retained{}, source_link_count_unchanged{};
};

// A pure planner for absent destinations. This distinct type is neither a
// namespace_regular replacement nor a mounted transaction or replay authority.
class Ext4RegularMovePlanner final
{
    using Block = Ext4RegularMoveBlock;
    struct Geometry
    {
        u32 seed{};
        std::array<u32, 4> block_bitmap{}, inode_bitmap{}, table{};
        std::array<u16, 4> flags{}, block_checksum{}, inode_checksum{};
    };
    struct Mapped
    {
        std::array<u32, 16> physical{};
        u32 count{};
    };
    struct Entry
    {
        usize offset{1012}, record{};
        u32 inode{};
    };

    static constexpr void store16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    static constexpr void store32(std::span<u8> bytes, usize at, u32 value)
    {
        for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8));
    }
    [[nodiscard]] static bool valid_name(std::string_view name)
    {
        if (name.empty() || name.size() > 31 || name == "." || name == "..") return false;
        for (char byte : name) if (!byte || byte == '/') return false;
        return true;
    }
    [[nodiscard]] static u16 descriptor_checksum(u32 seed, u32 group, std::span<const u8> data)
    {
        std::array<u8, 32> copy{};
        for (usize index = 0; index < copy.size(); ++index) copy[index] = data[index];
        copy[30] = copy[31] = 0;
        return u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), copy));
    }
    static void stamp_inode(std::span<u8> inode, u32 number, u32 seed)
    {
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 checksum = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        store16(inode, 124, u16(checksum)); store16(inode, 130, u16(checksum >> 16));
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
    [[nodiscard]] static Status inspect(const Ext4RecoveryInfo &binding, const Ext4RegularMoveSource &source,
                                        Geometry &geometry)
    {
        using namespace ext4_checked;
        const auto s = std::span<const u8>{source.superblock};
        if (binding.layout.block_size != 1024 || binding.layout.filesystem_blocks != 32768 ||
            binding.journal_inode != 8 || binding.layout.journal_blocks < 1024 ||
            binding.metadata_count != 16 || !binding.structural_metadata().valid() ||
            binding.layout.journal_first_block >= binding.layout.filesystem_blocks ||
            binding.layout.journal_blocks >
                binding.layout.filesystem_blocks - binding.layout.journal_first_block)
            return Status::unsupported("regular move planner requires bounded 32 MiB ext4 binding");
        if (jbd2::detail::crc32c(~u32{0}, s.first(1020)) != le32(s, 1020))
            return Status::io_error("regular move superblock checksum");
        if (le16(s, 56) != 0xef53 || le32(s, 24) || le32(s, 4) != 32768 ||
            le32(s, 0) != 8192 || le32(s, 20) != 1 || le32(s, 32) != 8192 ||
            le32(s, 40) != 2048 || le32(s, 84) < 11 || le16(s, 88) != 256 ||
            le32(s, 92) != 4 || le32(s, 96) != (binding.needs_recovery ? 0x46U : 0x42U) ||
            le32(s, 100) != 0x400 || le32(s, 224) != 8 || s[373] != 1)
            return Status::unsupported("regular move superblock profile");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (s[104 + index] != binding.uuid[index])
                return Status::io_error("regular move filesystem UUID differs");
        geometry.seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        if (!source.orphan.independently_checked_empty || source.orphan.observed_head ||
            source.orphan.filesystem_uuid != binding.uuid || le32(s, 232))
            return Status::unsupported("regular move prior orphan chain is not independently checked empty");
        u32 inode_free_total = 0;
        for (u32 group = 0; group < 4; ++group)
        {
            const auto d = std::span<const u8>{source.descriptors}.subspan(group * 32, 32);
            if (descriptor_checksum(geometry.seed, group, d) != le16(d, 30))
                return Status::io_error("regular move descriptor checksum");
            const u32 begin = 1 + group * 8192;
            const u32 count = group == 3 ? 8191 : 8192;
            const u32 block_bitmap = le32(d, 0), inode_bitmap = le32(d, 4), table = le32(d, 8);
            const std::array<jbd2::BlockRange, 4> expected{{{begin, 2}, {block_bitmap, 1},
                                                              {inode_bitmap, 1}, {table, 512}}};
            for (usize index = 0; index < expected.size(); ++index)
                if (binding.metadata[group * 4 + index].first != expected[index].first ||
                    binding.metadata[group * 4 + index].count != expected[index].count)
                    return Status::unsupported("regular move descriptor placement differs");
            geometry.block_bitmap[group] = block_bitmap;
            geometry.inode_bitmap[group] = inode_bitmap;
            geometry.table[group] = table;
            geometry.flags[group] = le16(d, 18);
            geometry.block_checksum[group] = le16(d, 24);
            geometry.inode_checksum[group] = le16(d, 26);
            if ((geometry.flags[group] & ~u16{7}) || le16(d, 12) > count ||
                le16(d, 14) > 2048 || le16(d, 16) > 2048 - le16(d, 14) ||
                le16(d, 28) > 2048 ||
                ((geometry.flags[group] & 1) && (le16(d, 14) != 2048 ||
                                                   le16(d, 16) || le16(d, 28) != 2048 ||
                                                   geometry.inode_checksum[group])) ||
                ((geometry.flags[group] & 2) && (le16(d, 12) != count - 516 ||
                                                   geometry.block_checksum[group])))
                return Status::io_error("regular move descriptor counters or flags");
            inode_free_total += le16(d, 14);
        }
        if (inode_free_total != le32(s, 16))
            return Status::io_error("regular move free inode counters disagree");
        return {};
    }

    [[nodiscard]] static Status allocated_inode(const Ext4RegularMoveSource &source,
                                                const Geometry &geometry, u32 number)
    {
        if (!number || number > 8192) return Status::invalid_argument("regular move inode number");
        const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
        if (geometry.flags[group] & 1)
            return Status::unsupported("regular move inode group is uninitialized");
        const auto bitmap = std::span<const u8>{source.inode_bitmaps[group]}.first(256);
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != geometry.inode_checksum[group])
            return Status::io_error("regular move inode bitmap checksum");
        if (!(bitmap[local / 8] & (1U << (local % 8))))
            return Status::io_error("regular move selected inode is not allocated");
        return {};
    }
    [[nodiscard]] static Status allocated_data(const Ext4RecoveryInfo &binding,
                                               const Ext4RegularMoveSource &source,
                                               const Geometry &geometry, u32 block)
    {
        if (!ordinary_block(binding, block) || (block == source.from_directory_block || block == source.to_directory_block))
            return Status::unsupported("regular move data aliases reserved or directory block");
        const u32 group = (block - 1) / 8192, local = (block - 1) % 8192;
        if (geometry.flags[group] & 2)
            return Status::unsupported("regular move block group is uninitialized");
        const auto bitmap = std::span<const u8>{source.block_bitmaps[group]};
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != geometry.block_checksum[group])
            return Status::io_error("regular move block bitmap checksum");
        if (!(bitmap[local / 8] & (1U << (local % 8))))
            return Status::io_error("regular move selected data block is not allocated");
        return {};
    }
    [[nodiscard]] static Status allocated_directory(const Ext4RegularMoveSource &source,
                                                    const Geometry &geometry, u32 block)
    {
        const u32 group = (block - 1) / 8192, local = (block - 1) % 8192;
        if (geometry.flags[group] & 2)
            return Status::unsupported("regular move directory block group is uninitialized");
        const auto bitmap = std::span<const u8>{source.block_bitmaps[group]};
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != geometry.block_checksum[group])
            return Status::io_error("regular move directory block bitmap checksum");
        if (!(bitmap[local / 8] & (1U << (local % 8))))
            return Status::io_error("regular move directory block is not allocated");
        return {};
    }
    [[nodiscard]] static u32 table_for(const Geometry &geometry, u32 number)
    { return geometry.table[(number - 1) / 2048] + ((number - 1) % 2048) / 4; }
    [[nodiscard]] static usize slot_for(u32 number)
    { return ((number - 1) % 4) * 256; }
    [[nodiscard]] static Status checked_inode(const Ext4RegularMoveSource &source,
                                              const Geometry &geometry, u32 number,
                                              u32 generation, const Block &table)
    {
        using namespace ext4_checked;
        if (auto status = allocated_inode(source, geometry, number); !status) return status;
        const auto inode = std::span<const u8>{table}.subspan(slot_for(number), 256);
        if (auto status = inode_checksum(geometry.seed, number, inode); !status) return status;
        if (le32(inode, 100) != generation)
            return Status::io_error("regular move inode generation differs");
        if (le16(inode, 128) != 32 || le32(inode, 20) ||
            le32(inode, 8) > 0x7fffffffU || le32(inode, 12) > 0x7fffffffU ||
            le32(inode, 16) > 0x7fffffffU ||
            (le32(inode, 140) >> 2) >= 1000000000U ||
            (le32(inode, 132) >> 2) >= 1000000000U ||
            (le32(inode, 136) >> 2) >= 1000000000U)
            return Status::unsupported("regular move inode timestamps or extra fields");
        return {};
    }
    [[nodiscard]] static Status regular(const Ext4RecoveryInfo &binding,
                                        const Ext4RegularMoveSource &source,
                                        const Geometry &geometry, u32 number,
                                        const Block &table, Mapped &mapped)
    {
        using namespace ext4_checked;
        const auto inode = std::span<const u8>{table}.subspan(slot_for(number), 256);
        const u32 size = le32(inode, 4);
        if ((le16(inode, 0) & 0170000) != 0100000 || (le16(inode, 0) & 07000) ||
            le16(inode, 26) != 1 || size > 16384 || le32(inode, 32) != 0x80000 ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) > 4 ||
            le16(inode, 44) != 4 || le16(inode, 46) || le32(inode, 48) ||
            le32(inode, 104) || le32(inode, 108) || le32(inode, 112) ||
            le16(inode, 116) || le16(inode, 118) || le16(inode, 126) ||
            le32(inode, 156))
            return Status::unsupported("regular move regular inode profile");
        for (usize at = 160; at < inode.size(); ++at)
            if (inode[at]) return Status::unsupported("regular move regular inode extra fields");
        const u32 expected = u32((u64{size} + 1023) / 1024);
        if (le32(inode, 28) != expected * 2 || expected > mapped.physical.size())
            return Status::unsupported("regular move regular block count");
        if (!size && le16(inode, 42))
            return Status::unsupported("regular move empty regular has mapped extent");
        for (usize at = 52 + le16(inode, 42) * 12; at < 100; ++at)
            if (inode[at]) return Status::unsupported("regular move unused extent bytes");
        for (u32 record = 0; record < le16(inode, 42); ++record)
        {
            const usize at = 52 + record * 12;
            const u32 logical = le32(inode, at), length = le16(inode, at + 4);
            const u32 physical = le32(inode, at + 8);
            if (!length || length > expected - mapped.count || logical != mapped.count ||
                le16(inode, at + 6) || !physical || physical > 32768 - length)
                return Status::unsupported("regular move requires initialized inline extents");
            for (u32 part = 0; part < length; ++part)
            {
                const u32 block = physical + part;
                if (auto status = allocated_data(binding, source, geometry, block); !status) return status;
                for (u32 previous = 0; previous < mapped.count; ++previous)
                    if (mapped.physical[previous] == block)
                        return Status::io_error("regular move regular extent aliases itself");
                mapped.physical[mapped.count++] = block;
            }
        }
        if (mapped.count != expected)
            return Status::unsupported("regular move regular extent coverage");
        return {};
    }
    [[nodiscard]] static Status parent_directory(u32 number, const Block &table, u32 physical)
    {
        using namespace ext4_checked;
        const auto inode = std::span<const u8>{table}.subspan(slot_for(number), 256);
        if ((le16(inode, 0) & 0170000) != 0040000 || (le16(inode, 0) & 0300) != 0300 ||
            le16(inode, 26) < 2 || le32(inode, 4) != 1024 || le32(inode, 28) != 2 ||
            le32(inode, 32) != 0x80000 || le16(inode, 40) != 0xf30a ||
            le16(inode, 42) != 1 || le16(inode, 44) != 4 || le16(inode, 46) ||
            le32(inode, 48) || le32(inode, 52) || le16(inode, 56) != 1 ||
            le16(inode, 58) || le32(inode, 60) != physical || le32(inode, 104) ||
            le32(inode, 108) || le32(inode, 112) || le16(inode, 116) ||
            le16(inode, 118) || le16(inode, 126) || le32(inode, 156))
            return Status::unsupported("regular move requires one bounded writable parent directory");
        for (usize at = 64; at < 100; ++at)
            if (inode[at]) return Status::unsupported("regular move unused parent extent bytes");
        for (usize at = 160; at < inode.size(); ++at)
            if (inode[at]) return Status::unsupported("regular move parent external inode fields");
        return {};
    }

    [[nodiscard]] static Status directory_entries(u32 seed, u32 parent_number, u32 source_number,
        const Block &parent_table, const Block &directory, std::string_view sought, bool from,
        Entry &entry, usize &aliases)
    {
        using namespace ext4_checked;
        constexpr usize tail = 1012;
        const auto parent = std::span<const u8>{parent_table}.subspan(slot_for(parent_number), 256);
        if (le32(directory, tail) || le16(directory, tail + 4) != 12 || directory[tail + 6] ||
            directory[tail + 7] != 0xde || le32(directory, tail + 8) != jbd2::detail::crc32c(
                inode_seed(seed, parent_number, parent), std::span<const u8>{directory}.first(tail)))
            return Status::io_error("regular move directory checksum");
        usize dots{}, parents{}, child_directories{};
        for (usize offset = 0; offset < tail;)
        {
            if (tail - offset < 8) return Status::io_error("regular move short dirent");
            const usize record = le16(directory, offset + 4), length = directory[offset + 6];
            const u32 number = le32(directory, offset);
            const u8 type = directory[offset + 7];
            if (record < 8 || record % 4 || record > tail - offset || length > record - 8 ||
                number > 8192 || (number && (!length || !type || type > 7)))
                return Status::io_error("regular move dirent bounds");
            if (number)
            {
                const std::string_view name{reinterpret_cast<const char *>(directory.data() + offset + 8), length};
                for (char byte : name)
                    if (!byte || byte == '/') return Status::io_error("regular move invalid dirent name");
                for (usize prior = 0; prior < offset; prior += le16(directory, prior + 4))
                    if (le32(directory, prior) && name == std::string_view{
                        reinterpret_cast<const char *>(directory.data() + prior + 8), directory[prior + 6]})
                        return Status::io_error("regular move duplicate dirent name");
                if (name == ".")
                {
                    if (offset || record != 12 || number != parent_number || type != 2)
                        return Status::io_error("regular move dot identity");
                    ++dots;
                }
                else if (name == "..")
                {
                    if (offset != 12 || number < 2 || number == source_number || type != 2 ||
                        (parent_number == 2 && number != 2))
                        return Status::io_error("regular move dotdot identity");
                    ++parents;
                }
                else
                {
                    if (number <= 10 || number == parent_number)
                        return Status::unsupported("regular move child identity");
                    if (type == 2) ++child_directories;
                }
                if (number == source_number) ++aliases;
                if (name == sought)
                {
                    if (from && (number != source_number || type != 1))
                        return Status::io_error("regular move source entry identity");
                    entry = {offset, record, number};
                }
            }
            offset += record;
        }
        if (dots != 1 || parents != 1 || le16(parent, 26) != child_directories + 2)
            return Status::io_error("regular move parent links or dot entries");
        return {};
    }

    [[nodiscard]] static Status append(Ext4RegularMovePlan &plan, u32 target,
        const Block &before, usize &index)
    {
        for (index = 0; index < plan.count; ++index)
            if (plan.targets[index] == target)
                return plan.before[index] == before ? Status{} :
                    Status::io_error("regular move aliased full preimages disagree");
        if (plan.count == plan.capacity) return Status::no_memory("regular move home capacity");
        index = plan.count++;
        plan.targets[index] = target; plan.before[index] = before; plan.after[index] = before;
        return {};
    }

    [[nodiscard]] static Status stamp_times(Block &table, u32 number, u32 seed,
        u32 seconds, u32 nanoseconds, bool parent)
    {
        using namespace ext4_checked;
        auto inode = std::span<u8>{table}.subspan(slot_for(number), 256);
        const u64 version = u64{le32(inode, 36)} | (u64{le32(inode, 152)} << 32);
        if (version == ~u64{0}) return Status::overflow("regular move inode version exhausted");
        store32(inode, 12, seconds); store32(inode, 132, nanoseconds << 2);
        if (parent) { store32(inode, 16, seconds); store32(inode, 136, nanoseconds << 2); }
        store32(inode, 36, u32(version + 1)); store32(inode, 152, u32((version + 1) >> 32));
        stamp_inode(inode, number, seed);
        return {};
    }

    static void stamp_directory(Block &directory, u32 seed, u32 parent_number, const Block &table)
    {
        const auto inode = std::span<const u8>{table}.subspan(slot_for(parent_number), 256);
        store32(directory, 1020, jbd2::detail::crc32c(ext4_checked::inode_seed(seed, parent_number, inode),
            std::span<const u8>{directory}.first(1012)));
    }

  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding, const Ext4RegularMoveUpdate &update,
        const Ext4RegularMoveSource &source, Ext4RegularMovePlan &output)
    {
        using namespace ext4_checked;
        // Reject an output whose reset would change an admitted input view.
        const usize output_address = reinterpret_cast<usize>(&output);
        const auto overlaps = [&](const void *data, usize bytes) {
            if (!bytes) return false;
            const usize address = reinterpret_cast<usize>(data);
            return (address >= output_address && address - output_address < sizeof(output)) ||
                (output_address >= address && output_address - address < bytes);
        };
        if (overlaps(&binding, sizeof(binding)) || overlaps(&update, sizeof(update)) ||
            overlaps(&source, sizeof(source)) || overlaps(update.old_name.data(), update.old_name.size()) ||
            overlaps(update.new_name.data(), update.new_name.size()) || overlaps(&source.superblock, sizeof(Block)) ||
            overlaps(&source.descriptors, sizeof(Block)) || overlaps(&source.from_parent_table, sizeof(Block)) ||
            overlaps(&source.to_parent_table, sizeof(Block)) || overlaps(&source.source_table, sizeof(Block)) ||
            overlaps(&source.from_directory, sizeof(Block)) || overlaps(&source.to_directory, sizeof(Block)) ||
            overlaps(&source.inode_bitmaps, sizeof(source.inode_bitmaps)) ||
            overlaps(&source.block_bitmaps, sizeof(source.block_bitmaps)) || overlaps(&source.orphan, sizeof(source.orphan)))
            return Status::invalid_argument("regular move output aliases input");
        output = {};
        if (update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U ||
            !valid_name(update.old_name) || !valid_name(update.new_name) ||
            update.from_parent < 2 || update.from_parent > 8192 || update.to_parent < 2 ||
            update.to_parent > 8192 || update.source <= 10 || update.source > 8192 ||
            update.from_parent == update.to_parent || update.from_parent == update.source ||
            update.to_parent == update.source)
            return Status::invalid_argument("regular move names, identities or timestamp");
        Geometry geometry{};
        if (auto status = inspect(binding, source, geometry); !status) return status;
        if (source.from_parent_table_block != table_for(geometry, update.from_parent) ||
            source.to_parent_table_block != table_for(geometry, update.to_parent) ||
            source.source_table_block != table_for(geometry, update.source) ||
            source.from_directory_block == source.to_directory_block ||
            !ordinary_block(binding, source.from_directory_block) || !ordinary_block(binding, source.to_directory_block))
            return Status::unsupported("regular move home placement");
        for (u32 block : {source.from_directory_block, source.to_directory_block})
        {
            if (block == source.from_parent_table_block || block == source.to_parent_table_block ||
                block == source.source_table_block) return Status::unsupported("regular move directory aliases inode table");
            if (auto status = allocated_directory(source, geometry, block); !status) return status;
        }
        if ((source.from_parent_table_block == source.to_parent_table_block && source.from_parent_table != source.to_parent_table) ||
            (source.from_parent_table_block == source.source_table_block && source.from_parent_table != source.source_table) ||
            (source.to_parent_table_block == source.source_table_block && source.to_parent_table != source.source_table))
            return Status::io_error("regular move aliased table views disagree");
        if (auto status = checked_inode(source, geometry, update.from_parent, update.from_generation, source.from_parent_table); !status) return status;
        if (auto status = checked_inode(source, geometry, update.to_parent, update.to_generation, source.to_parent_table); !status) return status;
        if (auto status = checked_inode(source, geometry, update.source, update.source_generation, source.source_table); !status) return status;
        if (auto status = parent_directory(update.from_parent, source.from_parent_table, source.from_directory_block); !status) return status;
        if (auto status = parent_directory(update.to_parent, source.to_parent_table, source.to_directory_block); !status) return status;
        Mapped data{};
        if (auto status = regular(binding, source, geometry, update.source, source.source_table, data); !status) return status;
        Entry from{}, to{}; usize from_aliases{}, to_aliases{};
        if (auto status = directory_entries(geometry.seed, update.from_parent, update.source, source.from_parent_table,
            source.from_directory, update.old_name, true, from, from_aliases); !status) return status;
        if (auto status = directory_entries(geometry.seed, update.to_parent, update.source, source.to_parent_table,
            source.to_directory, update.new_name, false, to, to_aliases); !status) return status;
        if (from.offset == 1012) return Status::not_found("regular move source absent");
        if (to.offset != 1012) return Status::already_exists("regular move destination exists");
        if (from_aliases != 1 || to_aliases) return Status::io_error("regular move source has another directory alias");

        const usize required = (8 + update.new_name.size() + 3) & ~usize{3};
        usize free_offset = 1012, slack_offset = 1012, donor_minimum{};
        for (usize offset = 0; offset < 1012; offset += le16(source.to_directory, offset + 4))
        {
            const usize record = le16(source.to_directory, offset + 4);
            const usize minimum = (8 + source.to_directory[offset + 6] + 3) & ~usize{3};
            if (!le32(source.to_directory, offset))
            {
                if (free_offset == 1012 && record >= required) free_offset = offset;
            }
            else if (slack_offset == 1012 && record - minimum >= required)
            { slack_offset = offset; donor_minimum = minimum; }
        }
        if (free_offset == 1012 && slack_offset == 1012)
            return Status::no_space("regular move destination has no fitting record");

        Ext4RegularMovePlan result{};
        usize from_dir{}, to_dir{}, from_table{}, to_table{}, file_table{};
        if (auto status = append(result, source.from_directory_block, source.from_directory, from_dir); !status) return status;
        if (auto status = append(result, source.to_directory_block, source.to_directory, to_dir); !status) return status;
        if (auto status = append(result, source.from_parent_table_block, source.from_parent_table, from_table); !status) return status;
        if (auto status = append(result, source.to_parent_table_block, source.to_parent_table, to_table); !status) return status;
        if (auto status = append(result, source.source_table_block, source.source_table, file_table); !status) return status;
        auto &removed = result.after[from_dir];
        store32(removed, from.offset, 0); removed[from.offset + 6] = removed[from.offset + 7] = 0;
        for (usize at = from.offset + 8; at < from.offset + from.record; ++at) removed[at] = 0;
        auto &inserted = result.after[to_dir];
        usize destination_offset = free_offset, destination_record{};
        if (free_offset != 1012) destination_record = le16(inserted, free_offset + 4);
        else
        {
            destination_offset = slack_offset + donor_minimum;
            destination_record = le16(inserted, slack_offset + 4) - donor_minimum;
            store16(inserted, slack_offset + 4, u16(donor_minimum));
        }
        for (usize at = destination_offset; at < destination_offset + destination_record; ++at) inserted[at] = 0;
        store32(inserted, destination_offset, update.source); store16(inserted, destination_offset + 4, u16(destination_record));
        inserted[destination_offset + 6] = u8(update.new_name.size()); inserted[destination_offset + 7] = 1;
        for (usize at = 0; at < update.new_name.size(); ++at) inserted[destination_offset + 8 + at] = u8(update.new_name[at]);
        if (auto status = stamp_times(result.after[from_table], update.from_parent, geometry.seed, update.seconds, update.nanoseconds, true); !status) return status;
        if (auto status = stamp_times(result.after[to_table], update.to_parent, geometry.seed, update.seconds, update.nanoseconds, true); !status) return status;
        if (auto status = stamp_times(result.after[file_table], update.source, geometry.seed, update.seconds, update.nanoseconds, false); !status) return status;
        stamp_directory(removed, geometry.seed, update.from_parent, result.after[from_table]);
        stamp_directory(inserted, geometry.seed, update.to_parent, result.after[to_table]);
        result.from_parent = update.from_parent; result.to_parent = update.to_parent; result.source = update.source;
        result.source_generation = update.source_generation;
        result.source_entry_offset = u16(from.offset); result.destination_entry_offset = u16(destination_offset);
        result.retained_data_blocks = data.physical; result.retained_data_count = data.count;
        result.old_name_length = update.old_name.size(); result.new_name_length = update.new_name.size();
        for (usize at = 0; at < update.old_name.size(); ++at) result.old_name[at] = update.old_name[at];
        for (usize at = 0; at < update.new_name.size(); ++at) result.new_name[at] = update.new_name[at];
        result.allocation_unchanged = result.file_identity_retained = result.source_link_count_unchanged = true;
        output = result;
        return {};
    }
};
} // namespace ok::fs
