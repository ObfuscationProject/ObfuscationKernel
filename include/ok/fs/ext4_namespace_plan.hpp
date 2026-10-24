#pragma once

#include "ok/fs/ext4_readonly.hpp"

namespace ok::fs
{
using Ext4NamespaceBlock = std::array<u8, 1024>;

enum class Ext4NamespaceOperation : u8 { replace, unlink };

struct Ext4NamespaceUpdate
{
    Ext4NamespaceOperation operation{Ext4NamespaceOperation::replace};
    u32 parent{}, source{}, destination{};
    u32 parent_generation{}, source_generation{}, destination_generation{};
    std::string_view source_name{}, destination_name{};
    u32 seconds{}, nanoseconds{};
};

// This is an explicit caller-supplied admission fact, not an orphan parser or
// an I/O capability. A future nonempty-chain proof requires a distinct type.
struct Ext4NamespaceEmptyOrphanAuthorization
{
    std::array<u8, 16> filesystem_uuid{};
    u32 observed_head{};
    bool independently_checked_empty{};
};

// Every image is from one exclusive, stable old view. Bitmap images remain
// read-only; the planner never releases an inode or data block.
struct Ext4NamespaceSource
{
    u32 parent_table_block{}, source_table_block{}, destination_table_block{}, directory_block{};
    const Ext4NamespaceBlock &superblock;
    const Ext4NamespaceBlock &descriptors;
    const Ext4NamespaceBlock &parent_table;
    const Ext4NamespaceBlock &source_table;
    const Ext4NamespaceBlock &destination_table;
    const Ext4NamespaceBlock &directory;
    const std::array<Ext4NamespaceBlock, 4> &inode_bitmaps;
    const std::array<Ext4NamespaceBlock, 4> &block_bitmaps;
    const Ext4NamespaceEmptyOrphanAuthorization &orphan;
};

struct Ext4NamespacePlan
{
    static constexpr usize capacity = 5;
    std::array<u32, capacity> targets{};
    std::array<Ext4NamespaceBlock, capacity> before{}, after{};
    usize count{};
    bool no_op{}, orphan_published{}, allocation_unchanged{};
    u32 removed_inode{}, old_orphan_head{};
};

// Pure bounded image planning only. In particular, a returned plan is not a
// mounted operation, transaction seal, replay proof or permission to write.
class Ext4NamespacePlanner final
{
    using Block = Ext4NamespaceBlock;
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
    [[nodiscard]] static Status inspect(const Ext4RecoveryInfo &binding, const Ext4NamespaceSource &source,
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
            return Status::unsupported("namespace planner requires bounded 32 MiB ext4 binding");
        if (jbd2::detail::crc32c(~u32{0}, s.first(1020)) != le32(s, 1020))
            return Status::io_error("namespace superblock checksum");
        if (le16(s, 56) != 0xef53 || le32(s, 24) || le32(s, 4) != 32768 ||
            le32(s, 0) != 8192 || le32(s, 20) != 1 || le32(s, 32) != 8192 ||
            le32(s, 40) != 2048 || le32(s, 84) < 11 || le16(s, 88) != 256 ||
            le32(s, 92) != 4 || le32(s, 96) != (binding.needs_recovery ? 0x46U : 0x42U) ||
            le32(s, 100) != 0x400 || le32(s, 224) != 8 || s[373] != 1)
            return Status::unsupported("namespace superblock profile");
        for (usize index = 0; index < binding.uuid.size(); ++index)
            if (s[104 + index] != binding.uuid[index])
                return Status::io_error("namespace filesystem UUID differs");
        geometry.seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        if (!source.orphan.independently_checked_empty || source.orphan.observed_head ||
            source.orphan.filesystem_uuid != binding.uuid || le32(s, 232))
            return Status::unsupported("namespace prior orphan chain is not independently checked empty");
        u32 inode_free_total = 0;
        for (u32 group = 0; group < 4; ++group)
        {
            const auto d = std::span<const u8>{source.descriptors}.subspan(group * 32, 32);
            if (descriptor_checksum(geometry.seed, group, d) != le16(d, 30))
                return Status::io_error("namespace descriptor checksum");
            const u32 begin = 1 + group * 8192;
            const u32 count = group == 3 ? 8191 : 8192;
            const u32 block_bitmap = le32(d, 0), inode_bitmap = le32(d, 4), table = le32(d, 8);
            const std::array<jbd2::BlockRange, 4> expected{{{begin, 2}, {block_bitmap, 1},
                                                              {inode_bitmap, 1}, {table, 512}}};
            for (usize index = 0; index < expected.size(); ++index)
                if (binding.metadata[group * 4 + index].first != expected[index].first ||
                    binding.metadata[group * 4 + index].count != expected[index].count)
                    return Status::unsupported("namespace descriptor placement differs");
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
                return Status::io_error("namespace descriptor counters or flags");
            inode_free_total += le16(d, 14);
        }
        if (inode_free_total != le32(s, 16))
            return Status::io_error("namespace free inode counters disagree");
        return {};
    }
    [[nodiscard]] static Status allocated_inode(const Ext4NamespaceSource &source,
                                                const Geometry &geometry, u32 number)
    {
        if (!number || number > 8192) return Status::invalid_argument("namespace inode number");
        const u32 group = (number - 1) / 2048, local = (number - 1) % 2048;
        if (geometry.flags[group] & 1)
            return Status::unsupported("namespace inode group is uninitialized");
        const auto bitmap = std::span<const u8>{source.inode_bitmaps[group]}.first(256);
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != geometry.inode_checksum[group])
            return Status::io_error("namespace inode bitmap checksum");
        if (!(bitmap[local / 8] & (1U << (local % 8))))
            return Status::io_error("namespace selected inode is not allocated");
        return {};
    }
    [[nodiscard]] static Status allocated_data(const Ext4RecoveryInfo &binding,
                                               const Ext4NamespaceSource &source,
                                               const Geometry &geometry, u32 block)
    {
        if (!ordinary_block(binding, block) || block == source.directory_block)
            return Status::unsupported("namespace data aliases reserved or directory block");
        const u32 group = (block - 1) / 8192, local = (block - 1) % 8192;
        if (geometry.flags[group] & 2)
            return Status::unsupported("namespace block group is uninitialized");
        const auto bitmap = std::span<const u8>{source.block_bitmaps[group]};
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != geometry.block_checksum[group])
            return Status::io_error("namespace block bitmap checksum");
        if (!(bitmap[local / 8] & (1U << (local % 8))))
            return Status::io_error("namespace selected data block is not allocated");
        return {};
    }
    [[nodiscard]] static Status allocated_directory(const Ext4NamespaceSource &source,
                                                    const Geometry &geometry)
    {
        const u32 block = source.directory_block;
        const u32 group = (block - 1) / 8192, local = (block - 1) % 8192;
        if (geometry.flags[group] & 2)
            return Status::unsupported("namespace directory block group is uninitialized");
        const auto bitmap = std::span<const u8>{source.block_bitmaps[group]};
        if (u16(jbd2::detail::crc32c(geometry.seed, bitmap)) != geometry.block_checksum[group])
            return Status::io_error("namespace directory block bitmap checksum");
        if (!(bitmap[local / 8] & (1U << (local % 8))))
            return Status::io_error("namespace directory block is not allocated");
        return {};
    }
    [[nodiscard]] static u32 table_for(const Geometry &geometry, u32 number)
    { return geometry.table[(number - 1) / 2048] + ((number - 1) % 2048) / 4; }
    [[nodiscard]] static usize slot_for(u32 number)
    { return ((number - 1) % 4) * 256; }
    [[nodiscard]] static Status checked_inode(const Ext4NamespaceSource &source,
                                              const Geometry &geometry, u32 number,
                                              u32 generation, const Block &table)
    {
        using namespace ext4_checked;
        if (auto status = allocated_inode(source, geometry, number); !status) return status;
        const auto inode = std::span<const u8>{table}.subspan(slot_for(number), 256);
        if (auto status = inode_checksum(geometry.seed, number, inode); !status) return status;
        if (le32(inode, 100) != generation)
            return Status::io_error("namespace inode generation differs");
        if (le16(inode, 128) != 32 || le32(inode, 20) ||
            le32(inode, 8) > 0x7fffffffU || le32(inode, 12) > 0x7fffffffU ||
            le32(inode, 16) > 0x7fffffffU ||
            (le32(inode, 140) >> 2) >= 1000000000U ||
            (le32(inode, 132) >> 2) >= 1000000000U ||
            (le32(inode, 136) >> 2) >= 1000000000U)
            return Status::unsupported("namespace inode timestamps or extra fields");
        return {};
    }
    [[nodiscard]] static Status regular(const Ext4RecoveryInfo &binding,
                                        const Ext4NamespaceSource &source,
                                        const Geometry &geometry, u32 number,
                                        const Block &table, Mapped &mapped)
    {
        using namespace ext4_checked;
        const auto inode = std::span<const u8>{table}.subspan(slot_for(number), 256);
        const u32 size = le32(inode, 4);
        if ((le16(inode, 0) & 0170000) != 0100000 || (le16(inode, 0) & 07000) ||
            le16(inode, 26) == 0 || size > 16384 || le32(inode, 32) != 0x80000 ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) > 4 ||
            le16(inode, 44) != 4 || le16(inode, 46) || le32(inode, 48) ||
            le32(inode, 104) || le32(inode, 108) || le32(inode, 112) ||
            le16(inode, 116) || le16(inode, 118) || le16(inode, 126) ||
            le32(inode, 156))
            return Status::unsupported("namespace regular inode profile");
        for (usize at = 160; at < inode.size(); ++at)
            if (inode[at]) return Status::unsupported("namespace regular inode extra fields");
        const u32 expected = u32((u64{size} + 1023) / 1024);
        if (le32(inode, 28) != expected * 2 || expected > mapped.physical.size())
            return Status::unsupported("namespace regular block count");
        if (!size && le16(inode, 42))
            return Status::unsupported("namespace empty regular has mapped extent");
        for (usize at = 52 + le16(inode, 42) * 12; at < 100; ++at)
            if (inode[at]) return Status::unsupported("namespace unused extent bytes");
        for (u32 record = 0; record < le16(inode, 42); ++record)
        {
            const usize at = 52 + record * 12;
            const u32 logical = le32(inode, at), length = le16(inode, at + 4);
            const u32 physical = le32(inode, at + 8);
            if (!length || length > expected - mapped.count || logical != mapped.count ||
                le16(inode, at + 6) || !physical || physical > 32768 - length)
                return Status::unsupported("namespace requires initialized inline extents");
            for (u32 part = 0; part < length; ++part)
            {
                const u32 block = physical + part;
                if (auto status = allocated_data(binding, source, geometry, block); !status) return status;
                for (u32 previous = 0; previous < mapped.count; ++previous)
                    if (mapped.physical[previous] == block)
                        return Status::io_error("namespace regular extent aliases itself");
                mapped.physical[mapped.count++] = block;
            }
        }
        if (mapped.count != expected)
            return Status::unsupported("namespace regular extent coverage");
        return {};
    }
    [[nodiscard]] static Status parent_directory(const Ext4NamespaceSource &source,
                                                 u32 number, const Block &table)
    {
        using namespace ext4_checked;
        const auto inode = std::span<const u8>{table}.subspan(slot_for(number), 256);
        if ((le16(inode, 0) & 0170000) != 0040000 ||
            (le16(inode, 0) & 0300) != 0300 || le16(inode, 26) < 2 ||
            le32(inode, 4) != 1024 || le32(inode, 28) != 2 ||
            le32(inode, 32) != 0x80000 || le16(inode, 40) != 0xf30a ||
            le16(inode, 42) != 1 || le16(inode, 44) != 4 || le16(inode, 46) ||
            le32(inode, 52) || le16(inode, 56) != 1 || le16(inode, 58) ||
            le32(inode, 60) != source.directory_block)
            return Status::unsupported("namespace parent needs one checked writable directory block");
        return {};
    }
    [[nodiscard]] static Status directory_entries(const Ext4NamespaceSource &source,
                                                  const Geometry &geometry,
                                                  const Ext4NamespaceUpdate &update,
                                                  const Block &parent_table,
                                                  Entry &from, Entry &to)
    {
        using namespace ext4_checked;
        constexpr usize tail = 1012;
        const auto directory = std::span<const u8>{source.directory};
        const auto parent = std::span<const u8>{parent_table}.subspan(slot_for(update.parent), 256);
        if (le32(directory, tail) || le16(directory, tail + 4) != 12 ||
            directory[tail + 6] || directory[tail + 7] != 0xde ||
            le32(directory, tail + 8) != jbd2::detail::crc32c(
                inode_seed(geometry.seed, update.parent, parent), directory.first(tail)))
            return Status::io_error("namespace directory tail checksum");
        bool dot = false, dotdot = false;
        for (usize offset = 0; offset < tail;)
        {
            if (tail - offset < 8) return Status::io_error("namespace short dirent");
            const usize record = le16(directory, offset + 4), length = directory[offset + 6];
            const u32 inode = le32(directory, offset);
            const u8 type = directory[offset + 7];
            if (record < 8 || record % 4 || record > tail - offset || length > record - 8 ||
                inode > 8192 || (inode && (!length || type > 7)))
                return Status::io_error("namespace dirent bounds");
            if (inode)
            {
                const std::string_view name{reinterpret_cast<const char *>(directory.data() + offset + 8), length};
                for (char byte : name)
                    if (!byte || byte == '/') return Status::io_error("namespace dirent name");
                if (name == ".")
                {
                    if (dot || inode != update.parent || type != 2)
                        return Status::io_error("namespace dot identity");
                    dot = true;
                }
                if (name == "..")
                {
                    if (dotdot || (update.parent == 2 && inode != 2) || type != 2)
                        return Status::io_error("namespace dotdot identity");
                    dotdot = true;
                }
                if (name == update.source_name)
                {
                    if (from.offset != tail || inode != update.source || type != 1)
                        return Status::io_error("namespace source dirent identity");
                    from = {offset, record, inode};
                }
                if (update.operation == Ext4NamespaceOperation::replace && name == update.destination_name)
                {
                    if (to.offset != tail || inode != update.destination || type != 1)
                        return Status::io_error("namespace destination dirent identity");
                    to = {offset, record, inode};
                }
                for (usize previous = 0; previous < offset;)
                {
                    const u32 other_number = le32(directory, previous);
                    const usize other_record = le16(directory, previous + 4);
                    const usize other_length = directory[previous + 6];
                    if (other_number && other_length == length &&
                        std::string_view{reinterpret_cast<const char *>(directory.data() + previous + 8),
                                         other_length} == name)
                        return Status::io_error("namespace duplicate dirent name");
                    previous += other_record;
                }
            }
            offset += record;
        }
        if (!dot || !dotdot || from.offset == tail ||
            (update.operation == Ext4NamespaceOperation::replace && to.offset == tail))
            return Status::not_found("namespace required directory entries absent");
        return {};
    }
    [[nodiscard]] static Status append(Ext4NamespacePlan &plan, u32 target,
                                       const Block &before, usize &index)
    {
        for (index = 0; index < plan.count; ++index)
            if (plan.targets[index] == target)
                return plan.before[index] == before ? Status{} :
                    Status::io_error("namespace aliased home images disagree");
        if (plan.count == plan.capacity) return Status::no_memory("namespace home capacity");
        index = plan.count++;
        plan.targets[index] = target;
        plan.before[index] = before;
        plan.after[index] = before;
        return {};
    }
    [[nodiscard]] static Status mutate_inode(Block &table, u32 number, u32 seed,
                                             u32 seconds, u32 nanoseconds,
                                             bool modify_time, bool remove_link, bool orphan)
    {
        using namespace ext4_checked;
        auto inode = std::span<u8>{table}.subspan(slot_for(number), 256);
        const u64 version = u64{le32(inode, 36)} | (u64{le32(inode, 152)} << 32);
        if (version == ~u64{0}) return Status::overflow("namespace inode version exhausted");
        store32(inode, 12, seconds); store32(inode, 132, nanoseconds << 2);
        if (modify_time)
        {
            store32(inode, 16, seconds); store32(inode, 136, nanoseconds << 2);
        }
        if (remove_link)
        {
            const u16 links = le16(inode, 26);
            if (!links) return Status::io_error("namespace removed inode already unlinked");
            store16(inode, 26, u16(links - 1));
        }
        if (orphan)
        {
            if (le32(inode, 20)) return Status::unsupported("namespace linked inode has orphan pointer");
            store32(inode, 20, 0); // The admitted previous head is exactly zero.
        }
        store32(inode, 36, u32(version + 1));
        store32(inode, 152, u32((version + 1) >> 32));
        stamp_inode(inode, number, seed);
        return {};
    }

  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
                                     const Ext4NamespaceUpdate &update,
                                     const Ext4NamespaceSource &source,
                                     Ext4NamespacePlan &output)
    {
        using namespace ext4_checked;
        output = {};
        if (update.operation != Ext4NamespaceOperation::replace &&
            update.operation != Ext4NamespaceOperation::unlink)
            return Status::invalid_argument("namespace operation");
        if (update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U ||
            !valid_name(update.source_name) || update.parent < 2 || update.parent > 8192 ||
            update.source < 11 || update.source > 8192 || update.parent == update.source ||
            (update.operation == Ext4NamespaceOperation::replace &&
             (!valid_name(update.destination_name) ||
              (update.source_name == update.destination_name && update.source != update.destination) ||
              update.destination < 11 ||
              update.destination > 8192 || update.parent == update.destination)) ||
            (update.operation == Ext4NamespaceOperation::unlink &&
             (update.destination || update.destination_generation ||
              !update.destination_name.empty())))
            return Status::invalid_argument("namespace names, identities or timestamp");
        Geometry geometry{};
        if (auto status = inspect(binding, source, geometry); !status) return status;
        const u32 destination = update.operation == Ext4NamespaceOperation::unlink ?
            update.source : update.destination;
        if (source.parent_table_block != table_for(geometry, update.parent) ||
            source.source_table_block != table_for(geometry, update.source) ||
            source.destination_table_block != table_for(geometry, destination) ||
            !ordinary_block(binding, source.directory_block) ||
            source.parent_table_block == source.directory_block ||
            source.source_table_block == source.directory_block ||
            source.destination_table_block == source.directory_block)
            return Status::unsupported("namespace home placement");
        if ((source.parent_table_block == source.source_table_block &&
             source.parent_table != source.source_table) ||
            (source.parent_table_block == source.destination_table_block &&
             source.parent_table != source.destination_table) ||
            (source.source_table_block == source.destination_table_block &&
             source.source_table != source.destination_table))
            return Status::io_error("namespace aliased table views differ");
        if (auto status = allocated_directory(source, geometry); !status) return status;
        if (auto status = checked_inode(source, geometry, update.parent,
                                        update.parent_generation, source.parent_table); !status) return status;
        if (auto status = checked_inode(source, geometry, update.source,
                                        update.source_generation, source.source_table); !status) return status;
        if (update.operation == Ext4NamespaceOperation::replace && update.destination != update.source)
            if (auto status = checked_inode(source, geometry, update.destination,
                                            update.destination_generation, source.destination_table); !status)
                return status;
        if (update.operation == Ext4NamespaceOperation::replace && update.destination == update.source &&
            (update.destination_generation != update.source_generation ||
             source.source_table != source.destination_table))
            return Status::io_error("namespace same-inode identity differs");
        if (auto status = parent_directory(source, update.parent, source.parent_table); !status) return status;
        Mapped source_blocks{}, destination_blocks{};
        if (auto status = regular(binding, source, geometry, update.source,
                                  source.source_table, source_blocks); !status) return status;
        if (update.operation == Ext4NamespaceOperation::replace && update.destination != update.source)
        {
            if (auto status = regular(binding, source, geometry, update.destination,
                                      source.destination_table, destination_blocks); !status) return status;
            for (u32 one = 0; one < source_blocks.count; ++one)
                for (u32 two = 0; two < destination_blocks.count; ++two)
                    if (source_blocks.physical[one] == destination_blocks.physical[two])
                        return Status::io_error("namespace source and victim alias data");
        }
        Entry from{}, to{};
        if (auto status = directory_entries(source, geometry, update,
                                            source.parent_table, from, to); !status) return status;
        if (update.operation == Ext4NamespaceOperation::replace && update.destination == update.source)
        {
            const auto inode = std::span<const u8>{source.source_table}.subspan(slot_for(update.source), 256);
            if (update.source_name != update.destination_name && le16(inode, 26) < 2)
                return Status::io_error("namespace same-inode links disagree");
            output.no_op = true;
            output.allocation_unchanged = true;
            return {};
        }
        const auto removed = std::span<const u8>{source.destination_table}.subspan(slot_for(destination), 256);
        const bool orphan = le16(removed, 26) == 1;
        if (!le16(removed, 26) || (orphan && le32(removed, 20)))
            return Status::unsupported("namespace removed inode links or orphan pointer");
        Ext4NamespacePlan proposed{};
        proposed.removed_inode = destination;
        proposed.old_orphan_head = 0;
        proposed.orphan_published = orphan;
        proposed.allocation_unchanged = true;
        usize parent_index{}, source_index{}, destination_index{}, directory_index{}, super_index{};
        if (auto status = append(proposed, source.parent_table_block,
                                 source.parent_table, parent_index); !status) return status;
        if (update.operation == Ext4NamespaceOperation::replace)
            if (auto status = append(proposed, source.source_table_block,
                                     source.source_table, source_index); !status) return status;
        if (auto status = append(proposed, source.destination_table_block,
                                 source.destination_table, destination_index); !status) return status;
        if (auto status = append(proposed, source.directory_block,
                                 source.directory, directory_index); !status) return status;
        if (orphan)
            if (auto status = append(proposed, 1, source.superblock, super_index); !status) return status;
        if (proposed.count > proposed.capacity || binding.layout.journal_blocks < proposed.count + 3)
            return Status::unsupported("namespace journal action capacity");
        if (auto status = mutate_inode(proposed.after[parent_index], update.parent, geometry.seed,
                                       update.seconds, update.nanoseconds, true, false, false); !status)
            return status;
        if (update.operation == Ext4NamespaceOperation::replace)
            if (auto status = mutate_inode(proposed.after[source_index], update.source, geometry.seed,
                                           update.seconds, update.nanoseconds, false, false, false); !status)
                return status;
        if (auto status = mutate_inode(proposed.after[destination_index], destination, geometry.seed,
                                       update.seconds, update.nanoseconds, false, true, orphan); !status)
            return status;
        auto &directory = proposed.after[directory_index];
        store32(directory, from.offset, 0);
        directory[from.offset + 6] = 0;
        directory[from.offset + 7] = 0;
        for (usize index = from.offset + 8; index < from.offset + from.record; ++index)
            directory[index] = 0;
        if (update.operation == Ext4NamespaceOperation::replace)
            store32(directory, to.offset, update.source);
        const auto parent = std::span<const u8>{source.parent_table}.subspan(slot_for(update.parent), 256);
        store32(directory, 1020, jbd2::detail::crc32c(
            inode_seed(geometry.seed, update.parent, parent),
            std::span<const u8>{directory}.first(1012)));
        if (orphan)
        {
            auto &superblock = proposed.after[super_index];
            store32(superblock, 232, destination);
            store32(superblock, 1020, jbd2::detail::crc32c(
                ~u32{0}, std::span<const u8>{superblock}.first(1020)));
        }
        output = proposed;
        return {};
    }
};
} // namespace ok::fs
