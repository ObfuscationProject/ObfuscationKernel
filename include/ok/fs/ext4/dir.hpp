#pragma once

// ext4 dir module: directory namespace, rename and regular-file move plans, witnesses and journal
// records.

#include "ok/fs/ext4/layout.hpp"
#include "ok/block/sessions.hpp"
#include <array>

namespace ok::fs
{
// Pure reservation arithmetic for the bounded 1 KiB directory mutation route.
// The continuing Owner independently retains its original Session attachment,
// observes exact phase endpoints and quarantines every attempted-write failure.
// A copied report or successful reserve call grants no transport authority.
class Ext4DirectoryInitializationBudget final
{
  public:
    // G<=32, filesystem<=262145 blocks, inodes/group<=8192, N<=128
    // allocated ownership slots, eight homes and nine canonical participants.
    // The journal's contiguous extent independently limits parsing to 32767
    // reads. The directory-only format probe precedes the initial Completion.
    // An already-zero candidate prepares strict proposed replay here; a
    // nonzero candidate defers that same replay to initialization. These phase
    // maxima are mutually exclusive and must not be added as one execution.
    static constexpr u64 preparation_reads = 28'495'267'196ULL;
    static constexpr u64 initialization_reads = 14'247'682'313ULL;
    static constexpr u64 checkpoint_reads = 14'247'648'694ULL;
    // Includes the conservative pre-Owner mounted name/metadata prefix, even
    // when the real reservation starts after it, and all post-checkpoint inode
    // validation before drained rearm. Root cold-opening allowances are separate.
    static constexpr u64 continuation_reads = 14'281'666'318ULL;
    // Maximum complete branch: nonzero preparation + nonzero initialization +
    // checkpoint + continuation. Strict proposed replay is charged once.
    static constexpr u64 whole_reads = 57'024'615'847ULL;
    static constexpr u64 whole_writes = 22;
    static constexpr u64 whole_flushes = 10;

    template <block::SessionCounterPolicy Policy = block::SessionCounter>
    [[nodiscard]] static Status reserve(const block::SessionReport &initial)
    {
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            !initial.identity || !initial.registration_generation || !initial.session_serial ||
            initial.in_flight || initial.pending_writes)
            return Status::fault("directory initialization requires a healthy original Session");
        const std::array<block::SessionCounter, 6> counts{
            initial.reads, initial.completed_reads, initial.writes,
            initial.completed_writes, initial.flushes, initial.completed_flushes};
        constexpr std::array<block::SessionCounter, 6> reserved{
            whole_reads, whole_reads, whole_writes, whole_writes, whole_flushes, whole_flushes};
        for (usize index = 0; index < counts.size(); ++index)
            if (!block::session_counter_fits<Policy>(counts[index], reserved[index]))
                return Status::overflow("directory initialization six-counter headroom exhausted");
        if (initial.reads != initial.completed_reads || initial.writes != initial.completed_writes ||
            initial.flushes != initial.completed_flushes)
            return Status::fault("directory initialization requires completed counter parity");
        return {};
    }
};

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

struct Ext4RegularMoveWitness
{
    Ext4RegularMoveUpdate update{};
    std::array<u32, Ext4RegularMovePlan::capacity> targets{};
    usize count{}, runs{}, bytes{};
};

// Reversible descriptor padding. The caller must validate the JBD2 descriptor
// CRC, recompute the planner and compare canonical images before recovery I/O.
// Neither this record nor a checksum grants device/session authority.
class Ext4RegularMoveWitnessCodec final
{
    using Block = Ext4RegularMoveBlock;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'M', 'V', 'W', '0', '0', '1'};

    static constexpr void store16(Block &block, usize at, u16 value)
    { block[at] = u8(value); block[at + 1] = u8(value >> 8); }
    static constexpr void store32(Block &block, usize at, u32 value)
    { for (usize byte = 0; byte < 4; ++byte) block[at + byte] = u8(value >> (8 * byte)); }
    [[nodiscard]] static bool name_valid(std::string_view name)
    {
        if (name.empty() || name.size() > 31 || name == "." || name == "..") return false;
        for (char byte : name) if (!byte || byte == '/') return false;
        return true;
    }
    [[nodiscard]] static bool update_valid(const Ext4RegularMoveUpdate &update)
    {
        return update.from_parent >= 2 && update.from_parent <= 8192 &&
            update.to_parent >= 2 && update.to_parent <= 8192 && update.source > 10 && update.source <= 8192 &&
            update.from_parent != update.to_parent && update.from_parent != update.source &&
            update.to_parent != update.source && update.seconds <= 0x7fffffffU &&
            update.nanoseconds < 1000000000U && name_valid(update.old_name) && name_valid(update.new_name);
    }
    [[nodiscard]] static bool overlap(const void *left, usize left_bytes, const void *right, usize right_bytes)
    {
        if (!left_bytes || !right_bytes) return false;
        const usize a = reinterpret_cast<usize>(left), b = reinterpret_cast<usize>(right);
        return (a >= b && a - b < right_bytes) || (b >= a && b - a < left_bytes);
    }
    [[nodiscard]] static bool zero(const Block &block, usize first, usize last)
    {
        for (usize at = first; at < last; ++at) if (block[at]) return false;
        return true;
    }

  public:
    static constexpr usize begin = 128, patch_begin = 320, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize at = 0; at < magic.size(); ++at)
            if (descriptor[begin + at] != magic[at]) return false;
        return true;
    }

    // Encoded names are copied. Every refusal preserves descriptor and inputs.
    [[nodiscard]] static Status encode(const Ext4RegularMoveUpdate &update,
                                       const Ext4RegularMovePlan &plan, Block &descriptor)
    {
        if (overlap(&descriptor, sizeof(descriptor), &plan, sizeof(plan)) ||
            overlap(&descriptor, sizeof(descriptor), &update, sizeof(update)) ||
            overlap(&descriptor, sizeof(descriptor), update.old_name.data(), update.old_name.size()) ||
            overlap(&descriptor, sizeof(descriptor), update.new_name.data(), update.new_name.size()))
            return Status::invalid_argument("regular move witness output aliases input");
        if (!update_valid(update) || plan.count < 3 || plan.count > Ext4RegularMovePlan::capacity ||
            update.from_parent != plan.from_parent || update.to_parent != plan.to_parent ||
            update.source != plan.source || update.source_generation != plan.source_generation ||
            !plan.allocation_unchanged || !plan.file_identity_retained || !plan.source_link_count_unchanged ||
            plan.old_name_length != update.old_name.size() || plan.new_name_length != update.new_name.size() ||
            std::string_view{plan.old_name.data(), plan.old_name_length} != update.old_name ||
            std::string_view{plan.new_name.data(), plan.new_name_length} != update.new_name)
            return Status::invalid_argument("regular move witness plan identity or names");
        for (usize home = 0; home < plan.count; ++home)
        {
            if (!plan.targets[home]) return Status::invalid_argument("regular move witness zero home");
            for (usize previous = 0; previous < home; ++previous)
                if (plan.targets[previous] == plan.targets[home])
                    return Status::invalid_argument("regular move witness duplicate home");
        }
        Block proposed = descriptor;
        for (usize at = 44 + (plan.count - 1) * 16; at < end; ++at) proposed[at] = 0;
        for (usize at = 0; at < magic.size(); ++at) proposed[begin + at] = magic[at];
        store16(proposed, 136, 1);
        proposed[140] = u8(plan.count); proposed[141] = u8(update.old_name.size()); proposed[142] = u8(update.new_name.size());
        store32(proposed, 144, update.from_parent); store32(proposed, 148, update.to_parent);
        store32(proposed, 152, update.source); store32(proposed, 156, update.from_generation);
        store32(proposed, 160, update.to_generation); store32(proposed, 164, update.source_generation);
        store32(proposed, 168, update.seconds); store32(proposed, 172, update.nanoseconds);
        for (usize at = 0; at < update.old_name.size(); ++at) proposed[240 + at] = u8(update.old_name[at]);
        for (usize at = 0; at < update.new_name.size(); ++at) proposed[272 + at] = u8(update.new_name[at]);
        usize cursor = patch_begin, runs = 0;
        for (usize home = 0; home < plan.count; ++home)
        {
            store32(proposed, 176 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            store32(proposed, 200 + home * 4, plan.targets[home]);
            for (usize at = 0; at < 1024;)
            {
                if (plan.before[home][at] == plan.after[home][at]) { ++at; continue; }
                const usize first = at;
                while (at < 1024 && plan.before[home][at] != plan.after[home][at]) ++at;
                const usize length = at - first;
                if (cursor > end || end - cursor < 5 || length > end - cursor - 5)
                    return Status::unsupported("regular move complete preimage witness exceeds descriptor capacity");
                proposed[cursor] = u8(home); store16(proposed, cursor + 1, u16(first)); store16(proposed, cursor + 3, u16(length));
                cursor += 5;
                for (usize byte = 0; byte < length; ++byte) proposed[cursor++] = plan.before[home][first + byte];
                ++runs;
            }
        }
        store16(proposed, 138, u16(cursor)); store16(proposed, 304, u16(runs));
        descriptor = proposed;
        return {};
    }

    // Names borrow immutable descriptor storage. All before blocks must be
    // disjoint from every input and from the witness output. Aliasing refusals
    // and oversized-span refusals happen before either output is reset. Later
    // failure clears the witness; partial blocks grant no recovery authority.
    [[nodiscard]] static Status decode(const Block &descriptor, std::span<const Block> after,
                                       std::span<Block> before, Ext4RegularMoveWitness &output)
    {
        using namespace ext4_checked;
        if (after.size() > Ext4RegularMovePlan::capacity || before.size() > Ext4RegularMovePlan::capacity)
            return Status::invalid_argument("regular move witness span capacity");
        if (overlap(before.data(), before.size_bytes(), after.data(), after.size_bytes()) ||
            overlap(before.data(), before.size_bytes(), &descriptor, sizeof(descriptor)) ||
            overlap(before.data(), before.size_bytes(), &output, sizeof(output)) ||
            overlap(&output, sizeof(output), after.data(), after.size_bytes()) ||
            overlap(&output, sizeof(output), &descriptor, sizeof(descriptor)))
            return Status::invalid_argument("regular move witness decode outputs alias input");
        output = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("regular move durable witness absent or unknown");
        const usize count = descriptor[140], old_length = descriptor[141], new_length = descriptor[142];
        const usize limit = le16(descriptor, 138), runs = le16(descriptor, 304);
        if (count < 3 || count > Ext4RegularMovePlan::capacity || after.size() != count || before.size() != count ||
            !old_length || old_length > 31 || !new_length || new_length > 31 || limit < patch_begin || limit > end)
            return Status::invalid_argument("regular move witness header geometry");
        if (!zero(descriptor, 44 + (count - 1) * 16, begin) || descriptor[143] ||
            !zero(descriptor, 176 + count * 4, 200) || !zero(descriptor, 200 + count * 4, 240) ||
            !zero(descriptor, 240 + old_length, 272) || !zero(descriptor, 272 + new_length, 304) ||
            !zero(descriptor, 306, patch_begin) || !zero(descriptor, limit, end))
            return Status::io_error("regular move witness padding differs");
        Ext4RegularMoveWitness decoded{};
        decoded.update = {le32(descriptor, 144), le32(descriptor, 148), le32(descriptor, 152),
            le32(descriptor, 156), le32(descriptor, 160), le32(descriptor, 164),
            {reinterpret_cast<const char *>(descriptor.data() + 240), old_length},
            {reinterpret_cast<const char *>(descriptor.data() + 272), new_length},
            le32(descriptor, 168), le32(descriptor, 172)};
        if (!update_valid(decoded.update)) return Status::invalid_argument("regular move witnessed identity or names");
        for (usize home = 0; home < count; ++home)
        {
            decoded.targets[home] = le32(descriptor, 200 + home * 4);
            if (!decoded.targets[home]) return Status::io_error("regular move witnessed zero home");
            for (usize previous = 0; previous < home; ++previous)
                if (decoded.targets[previous] == decoded.targets[home])
                    return Status::io_error("regular move witnessed duplicate home");
        }
        for (usize home = 0; home < count; ++home) before[home] = after[home];
        usize cursor = patch_begin, previous_home = 0, previous_end = 0;
        for (usize run = 0; run < runs; ++run)
        {
            if (cursor > limit || limit - cursor < 5) return Status::io_error("regular move witness truncated patch");
            const usize home = descriptor[cursor], offset = le16(descriptor, cursor + 1), length = le16(descriptor, cursor + 3);
            cursor += 5;
            if (home >= count || offset >= 1024 || !length || length > 1024 - offset || length > limit - cursor ||
                (run && (home < previous_home || (home == previous_home && offset <= previous_end))))
                return Status::io_error("regular move witness patch bounds or order");
            for (usize byte = 0; byte < length; ++byte)
            {
                if (descriptor[cursor + byte] == after[home][offset + byte])
                    return Status::io_error("regular move witness patch includes unchanged byte");
                before[home][offset + byte] = descriptor[cursor + byte];
            }
            cursor += length; previous_home = home; previous_end = offset + length;
        }
        if (cursor != limit) return Status::io_error("regular move witness patch length differs");
        for (usize home = 0; home < count; ++home)
            if (le32(descriptor, 176 + home * 4) != jbd2::detail::crc32c(~u32{0}, before[home]))
                return Status::io_error("regular move complete preimage checksum differs");
        decoded.count = count; decoded.runs = runs; decoded.bytes = limit - begin;
        output = decoded;
        return {};
    }
};

struct Ext4NamespaceJournalScratch
{
    Ext4NamespacePlan namespace_plan{};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4NamespaceBlock record{}, payload{};
};
struct Ext4NamespaceJournalPlan
{
    Ext4NamespaceBlock journal_head{}, descriptor{}, commit{}, end{};
    std::array<Ext4NamespaceBlock, 5> payloads{};
    std::array<u32, 5> targets{};
    usize count{};
    u32 sequence{};
    bool no_op{}, ready{};
};

// Immutable format preparation only. Scratch/output must be disjoint from
// source images and each other. No I/O, sealing, replay or mount authority.
class Ext4NamespaceJournalPlanner final
{
    using Block = Ext4NamespaceBlock;
    static void store(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void header(Block &bytes, jbd2::RecordType type, u32 sequence)
    { store(bytes, 0, jbd2::magic); store(bytes, 4, u32(type)); store(bytes, 8, sequence); }
    struct Records
    {
        const Ext4NamespaceJournalPlan &plan;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("namespace journal model read size");
            const Block *record = nullptr;
            if (number == 0) record = &plan.journal_head;
            else if (number == 1) record = &plan.descriptor;
            else if (number >= 2 && number < plan.count + 2) record = &plan.payloads[number - 2];
            else if (number == plan.count + 2) record = &plan.commit;
            else if (number == plan.count + 3) record = &plan.end;
            if (!record) return Status::io_error("namespace journal model read bounds");
            for (usize index = 0; index < bytes.size(); ++index) bytes[index] = (*record)[index];
            return {};
        }
    };
  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4NamespaceUpdate &update, const Ext4NamespaceSource &source,
        const Block &old_journal_head, Ext4NamespaceJournalScratch &scratch,
        Ext4NamespaceJournalPlan &output)
    {
        output = {}; scratch = {};
        const auto reject = [&](Status status) { output = {}; return status; };
        if (!binding.needs_recovery || !binding.empty_log)
            return Status::unsupported("namespace journal requires mounted dirty empty log");
        const auto codec = jbd2::Codec::open(old_journal_head, u32(binding.layout.journal_blocks),
            binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &journal = codec.value().superblock();
        if (journal.block_size != 1024 || journal.first_block != 1 || journal.start_block ||
            journal.sequence != binding.journal_sequence || journal.sequence > 0xfffffffcU ||
            (journal.features != jbd2::feature_checksum_v3 &&
             journal.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return Status::unsupported("namespace journal ring, sequence or feature profile");
        auto &canonical = scratch.namespace_plan;
        if (const auto status = Ext4NamespacePlanner::plan(binding, update, source, canonical); !status)
            return status;
        if (canonical.no_op)
        { output.no_op = true; output.ready = true; return {}; }
        if (canonical.count < 2 || canonical.count > output.targets.size() ||
            journal.max_blocks <= canonical.count + 3)
            return Status::unsupported("namespace journal record capacity");
        output.count = canonical.count; output.sequence = journal.sequence + 1;
        output.journal_head = old_journal_head;
        header(output.descriptor, jbd2::RecordType::descriptor, output.sequence);
        header(output.commit, jbd2::RecordType::commit, output.sequence);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const std::array<u8, 4> sequence{u8(output.sequence >> 24), u8(output.sequence >> 16),
                                      u8(output.sequence >> 8), u8(output.sequence)};
        for (usize index = 0; index < output.count; ++index)
        {
            output.targets[index] = canonical.targets[index];
            auto &payload = output.payloads[index];
            payload = canonical.after[index];
            u32 flags = index ? jbd2::flag_same_uuid : 0;
            if (index + 1 == output.count) flags |= jbd2::flag_last_tag;
            if (jbd2::detail::be32(payload, 0) == jbd2::magic)
            { store(payload, 0, 0); flags |= jbd2::flag_escape; }
            const usize at = index == 0 ? 12 : 44 + (index - 1) * 16;
            store(output.descriptor, at, output.targets[index]);
            store(output.descriptor, at + 4, flags);
            store(output.descriptor, at + 12,
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), payload));
        }
        store(output.descriptor, 1020, jbd2::detail::crc32c(seed, output.descriptor));
        store(output.commit, 52, update.seconds); store(output.commit, 56, update.nanoseconds);
        store(output.commit, 16, jbd2::detail::crc32c(seed, output.commit));
        store(output.journal_head, 24, output.sequence); store(output.journal_head, 28, 1);
        store(output.journal_head, 252, 0);
        store(output.journal_head, 252, jbd2::detail::crc32c(~u32{0}, output.journal_head));
        const auto new_codec = jbd2::Codec::open(output.journal_head, u32(binding.layout.journal_blocks),
            binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!new_codec) return reject(new_codec.status());
        std::array<jbd2::BlockRange, 5> allowed{};
        for (usize index = 0; index < output.count; ++index) allowed[index] = {output.targets[index], 1};
        Records records{output};
        const auto parsed = jbd2::build_replay_plan(new_codec.value(), records,
            {binding.layout.filesystem_blocks, std::span<const jbd2::BlockRange>{allowed}.first(output.count)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return reject(parsed.status());
        const auto &replay = parsed.value();
        if (replay.actions.size() != output.count || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.incomplete_tail || replay.empty_log ||
            replay.next_sequence != output.sequence + 1)
            return reject(Status::fault("namespace journal generated record shape"));
        for (usize index = 0; index < replay.actions.size(); ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != output.targets[index] || action.journal_block != index + 2 ||
                action.sequence != output.sequence || action.transaction_order)
                return reject(Status::fault("namespace journal generated target identity"));
            const auto &payload = output.payloads[index];
            for (usize byte = 0; byte < payload.size(); ++byte)
            {
                const u8 restored = action.restore_magic && byte < 4 ? u8(jbd2::magic >> (24 - byte * 8)) : payload[byte];
                if (restored != canonical.after[index][byte])
                    return reject(Status::fault("namespace journal generated payload differs"));
            }
        }
        output.ready = true;
        return {};
    }
};

struct Ext4NamespaceWitness
{
    Ext4NamespaceUpdate update{};
    usize count{}, runs{}, bytes{};
};

// Private reversible descriptor extension. Neither decoded bytes nor a CRC
// attest device/session provenance or authorize recovery I/O.
class Ext4NamespaceWitnessCodec final
{
    using Block = Ext4NamespaceBlock;
    static constexpr std::array<u8, 8> magic{'O', 'K', 'N', 'S', 'W', '0', '0', '1'};
    static void store16(Block &block, usize at, u16 value)
    { block[at] = u8(value); block[at + 1] = u8(value >> 8); }
    static void store32(Block &block, usize at, u32 value)
    { for (usize i = 0; i < 4; ++i) block[at + i] = u8(value >> (8 * i)); }
  public:
    static constexpr usize begin = 128, patch_begin = 264, end = 1020;
    [[nodiscard]] static bool present(const Block &descriptor)
    {
        for (usize i = 0; i < magic.size(); ++i)
            if (descriptor[begin + i] != magic[i]) return false;
        return true;
    }
    // Leaves descriptor unchanged if the full witness does not fit.
    [[nodiscard]] static Status encode(const Ext4NamespaceUpdate &update,
        const Ext4NamespacePlan &plan, Block &descriptor)
    {
        if (plan.no_op || plan.count < 2 || plan.count > 5 ||
            update.source_name.empty() || update.source_name.size() > 31 ||
            update.destination_name.size() > 31 ||
            (update.operation != Ext4NamespaceOperation::replace &&
             update.operation != Ext4NamespaceOperation::unlink))
            return Status::invalid_argument("namespace witness plan or names");
        Block proposed = descriptor;
        for (usize at = begin; at < end; ++at) proposed[at] = 0;
        for (usize i = 0; i < magic.size(); ++i) proposed[begin + i] = magic[i];
        store16(proposed, 136, 1);
        proposed[140] = u8(plan.count); proposed[141] = u8(update.operation);
        store32(proposed, 144, update.parent); store32(proposed, 148, update.source);
        store32(proposed, 152, update.destination);
        store32(proposed, 156, update.parent_generation);
        store32(proposed, 160, update.source_generation);
        store32(proposed, 164, update.destination_generation);
        store32(proposed, 168, update.seconds); store32(proposed, 172, update.nanoseconds);
        proposed[176] = u8(update.source_name.size());
        proposed[177] = u8(update.destination_name.size());
        for (usize i = 0; i < update.source_name.size(); ++i)
            proposed[200 + i] = u8(update.source_name[i]);
        for (usize i = 0; i < update.destination_name.size(); ++i)
            proposed[232 + i] = u8(update.destination_name[i]);
        usize cursor = patch_begin, runs = 0;
        for (usize home = 0; home < plan.count; ++home)
        {
            store32(proposed, 180 + home * 4, jbd2::detail::crc32c(~u32{0}, plan.before[home]));
            for (usize at = 0; at < 1024;)
            {
                if (plan.before[home][at] == plan.after[home][at]) { ++at; continue; }
                const usize first = at;
                while (at < 1024 && plan.before[home][at] != plan.after[home][at]) ++at;
                const usize length = at - first;
                if (cursor > end || end - cursor < 5 || length > end - cursor - 5)
                    return Status::unsupported("namespace complete before witness exceeds descriptor capacity");
                proposed[cursor] = u8(home);
                store16(proposed, cursor + 1, u16(first));
                store16(proposed, cursor + 3, u16(length));
                cursor += 5;
                for (usize i = 0; i < length; ++i) proposed[cursor++] = plan.before[home][first + i];
                ++runs;
            }
        }
        store16(proposed, 138, u16(cursor)); store16(proposed, 142, u16(runs));
        descriptor = proposed;
        return {};
    }
    // Names borrow descriptor storage. before, after and descriptor must not
    // alias; the caller must keep descriptor immutable while using the result.
    [[nodiscard]] static Status decode(const Block &descriptor,
        std::span<const Block> after, std::span<Block> before, Ext4NamespaceWitness &output)
    {
        using namespace ext4_checked;
        output = {};
        if (!present(descriptor) || le16(descriptor, 136) != 1)
            return Status::unsupported("namespace durable witness absent or version unknown");
        const usize count = descriptor[140], limit = le16(descriptor, 138), runs = le16(descriptor, 142);
        if (count < 2 || count > 5 || after.size() != count || before.size() != count ||
            limit < patch_begin || limit > end || descriptor[141] > u8(Ext4NamespaceOperation::unlink) ||
            !descriptor[176] || descriptor[176] > 31 || descriptor[177] > 31 ||
            descriptor[178] || descriptor[179])
            return Status::invalid_argument("namespace witness header geometry");
        for (usize at = limit; at < end; ++at)
            if (descriptor[at]) return Status::io_error("namespace witness trailing bytes");
        for (usize at = 180 + count * 4; at < 200; ++at)
            if (descriptor[at]) return Status::io_error("namespace witness unused preimage CRC");
        for (usize at = 200 + descriptor[176]; at < 232; ++at)
            if (descriptor[at]) return Status::io_error("namespace witness source name padding");
        for (usize at = 232 + descriptor[177]; at < patch_begin; ++at)
            if (descriptor[at]) return Status::io_error("namespace witness destination name padding");
        for (usize home = 0; home < count; ++home) before[home] = after[home];
        usize cursor = patch_begin, prior_home = 0, prior_end = 0;
        for (usize run = 0; run < runs; ++run)
        {
            if (cursor > limit || limit - cursor < 5)
                return Status::io_error("namespace witness truncated patch");
            const usize home = descriptor[cursor], offset = le16(descriptor, cursor + 1);
            const usize length = le16(descriptor, cursor + 3);
            cursor += 5;
            if (home >= count || offset >= 1024 || !length || length > 1024 - offset ||
                length > limit - cursor ||
                (run && (home < prior_home || (home == prior_home && offset <= prior_end))))
                return Status::io_error("namespace witness patch bounds or canonical order");
            for (usize i = 0; i < length; ++i)
            {
                if (descriptor[cursor + i] == after[home][offset + i])
                    return Status::io_error("namespace witness patch includes unchanged byte");
                before[home][offset + i] = descriptor[cursor + i];
            }
            cursor += length; prior_home = home; prior_end = offset + length;
        }
        if (cursor != limit) return Status::io_error("namespace witness patch length mismatch");
        for (usize home = 0; home < count; ++home)
            if (le32(descriptor, 180 + home * 4) != jbd2::detail::crc32c(~u32{0}, before[home]))
                return Status::io_error("namespace witnessed preimage checksum");
        output.update = {Ext4NamespaceOperation(descriptor[141]), le32(descriptor, 144),
            le32(descriptor, 148), le32(descriptor, 152), le32(descriptor, 156),
            le32(descriptor, 160), le32(descriptor, 164),
            {reinterpret_cast<const char *>(descriptor.data() + 200), descriptor[176]},
            {reinterpret_cast<const char *>(descriptor.data() + 232), descriptor[177]},
            le32(descriptor, 168), le32(descriptor, 172)};
        output.count = count; output.runs = runs; output.bytes = limit - begin;
        return {};
    }
};

// The witness is flushed with the existing descriptor before commit. This
// class prepares bytes only; legacy witness-free planning remains separate.
class Ext4NamespaceDurableJournalPlanner final
{
  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4NamespaceUpdate &update, const Ext4NamespaceSource &source,
        const Ext4NamespaceBlock &old_head, Ext4NamespaceJournalScratch &scratch,
        Ext4NamespaceJournalPlan &output)
    {
        if (auto status = Ext4NamespaceJournalPlanner::plan(binding, update, source, old_head, scratch, output);
            !status) return status;
        if (output.no_op) return {};
        if (auto status = Ext4NamespaceWitnessCodec::encode(update, scratch.namespace_plan, output.descriptor);
            !status) { output = {}; return status; }
        for (usize at = 1020; at < 1024; ++at) output.descriptor[at] = 0;
        const u32 crc = jbd2::detail::crc32c(jbd2::detail::crc32c(~u32{0}, binding.uuid), output.descriptor);
        for (usize i = 0; i < 4; ++i) output.descriptor[1020 + i] = u8(crc >> (24 - i * 8));
        return {};
    }
};

struct Ext4RegularMoveJournalScratch
{
    Ext4RegularMovePlan regular_move_plan{};
    std::array<jbd2::ReplayAction, 5> actions{};
    std::array<jbd2::Revocation, 1> revocations{};
    Ext4RegularMoveBlock record{}, payload{};
    std::array<Ext4RegularMoveBlock, Ext4RegularMovePlan::capacity> witness_before{};
    Ext4RegularMoveWitness decoded_witness{};
};
struct Ext4RegularMoveJournalPlan
{
    Ext4RegularMoveBlock journal_head{}, descriptor{}, commit{}, end{};
    std::array<Ext4RegularMoveBlock, 5> payloads{};
    std::array<u32, 5> targets{};
    usize count{};
    u32 sequence{};
    bool ready{};
};

// Immutable format preparation only. Scratch/output must be disjoint from
// source images and each other. Structural JBD2 admission and witness roundtrip
// are construction checks; no strict replay, I/O, sealing or mount authority.
class Ext4RegularMoveJournalPlanner final
{
    using Block = Ext4RegularMoveBlock;
    static void store(std::span<u8> bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void header(Block &bytes, jbd2::RecordType type, u32 sequence)
    { store(bytes, 0, jbd2::magic); store(bytes, 4, u32(type)); store(bytes, 8, sequence); }
    struct Records
    {
        const Ext4RegularMoveJournalPlan &plan;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (bytes.size() != 1024) return Status::invalid_argument("regular move journal model read size");
            const Block *record = nullptr;
            if (number == 0) record = &plan.journal_head;
            else if (number == 1) record = &plan.descriptor;
            else if (number >= 2 && number < plan.count + 2) record = &plan.payloads[number - 2];
            else if (number == plan.count + 2) record = &plan.commit;
            else if (number == plan.count + 3) record = &plan.end;
            if (!record) return Status::io_error("regular move journal model read bounds");
            for (usize index = 0; index < bytes.size(); ++index) bytes[index] = (*record)[index];
            return {};
        }
    };
  public:
    [[nodiscard]] static Status plan(const Ext4RecoveryInfo &binding,
        const Ext4RegularMoveUpdate &update, const Ext4RegularMoveSource &source,
        const Block &old_journal_head, Ext4RegularMoveJournalScratch &scratch,
        Ext4RegularMoveJournalPlan &output)
    {
        // Both resets must preserve every input, including borrowed names.
        const auto overlap = [](const void *one, usize a, const void *two, usize b) {
            if (!a || !b) return false;
            const usize x = reinterpret_cast<usize>(one), y = reinterpret_cast<usize>(two);
            return (x >= y && x - y < b) || (y >= x && y - x < a);
        };
        if (overlap(&scratch, sizeof(scratch), &output, sizeof(output)))
            return Status::invalid_argument("regular move journal scratch/output overlap");
        const auto aliases = [&](const void *data, usize bytes) {
            return overlap(&scratch, sizeof(scratch), data, bytes) ||
                overlap(&output, sizeof(output), data, bytes);
        };
        if (aliases(&binding, sizeof(binding)) || aliases(&update, sizeof(update)) ||
            aliases(&source, sizeof(source)) || aliases(&old_journal_head, sizeof(Block)) ||
            aliases(update.old_name.data(), update.old_name.size()) ||
            aliases(update.new_name.data(), update.new_name.size()) ||
            aliases(&source.superblock, sizeof(Block)) || aliases(&source.descriptors, sizeof(Block)) ||
            aliases(&source.from_parent_table, sizeof(Block)) || aliases(&source.to_parent_table, sizeof(Block)) ||
            aliases(&source.source_table, sizeof(Block)) || aliases(&source.from_directory, sizeof(Block)) ||
            aliases(&source.to_directory, sizeof(Block)) || aliases(&source.inode_bitmaps, sizeof(source.inode_bitmaps)) ||
            aliases(&source.block_bitmaps, sizeof(source.block_bitmaps)) || aliases(&source.orphan, sizeof(source.orphan)))
            return Status::invalid_argument("regular move journal output aliases input");
        output = {}; scratch = {};
        const auto reject = [&](Status status) { output = {}; return status; };
        if (!binding.needs_recovery || !binding.empty_log)
            return Status::unsupported("regular move journal requires mounted dirty empty log");
        const auto codec = jbd2::Codec::open(old_journal_head, u32(binding.layout.journal_blocks),
            binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &journal = codec.value().superblock();
        if (journal.block_size != 1024 || journal.max_blocks != binding.layout.journal_blocks ||
            journal.first_block != 1 || journal.start_block ||
            journal.sequence != binding.journal_sequence || journal.sequence > 0xfffffffcU ||
            (journal.features != jbd2::feature_checksum_v3 &&
             journal.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return Status::unsupported("regular move journal ring, sequence or feature profile");
        auto &canonical = scratch.regular_move_plan;
        if (const auto status = Ext4RegularMovePlanner::plan(binding, update, source, canonical); !status)
            return status;
        if (canonical.count < 3 || canonical.count > output.targets.size() ||
            journal.max_blocks <= canonical.count + 3)
            return Status::unsupported("regular move journal record capacity");
        output.count = canonical.count; output.sequence = journal.sequence + 1;
        output.journal_head = old_journal_head;
        header(output.descriptor, jbd2::RecordType::descriptor, output.sequence);
        header(output.commit, jbd2::RecordType::commit, output.sequence);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        const std::array<u8, 4> sequence{u8(output.sequence >> 24), u8(output.sequence >> 16),
                                      u8(output.sequence >> 8), u8(output.sequence)};
        for (usize index = 0; index < output.count; ++index)
        {
            output.targets[index] = canonical.targets[index];
            auto &payload = output.payloads[index];
            payload = canonical.after[index];
            u32 flags = index ? jbd2::flag_same_uuid : 0;
            if (index + 1 == output.count) flags |= jbd2::flag_last_tag;
            if (jbd2::detail::be32(payload, 0) == jbd2::magic)
            { store(payload, 0, 0); flags |= jbd2::flag_escape; }
            const usize at = index == 0 ? 12 : 44 + (index - 1) * 16;
            store(output.descriptor, at, output.targets[index]);
            store(output.descriptor, at + 4, flags);
            store(output.descriptor, at + 12,
                jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), payload));
        }
        // Only OKMVW001 supplies complete inverse preimages for this family.
        // The marker and every patch byte participate in the descriptor CRC.
        if (const auto status = Ext4RegularMoveWitnessCodec::encode(update, canonical, output.descriptor); !status)
            return reject(status);
        if (const auto status = Ext4RegularMoveWitnessCodec::decode(output.descriptor,
                std::span<const Block>{canonical.after}.first(canonical.count),
                std::span<Block>{scratch.witness_before}.first(canonical.count), scratch.decoded_witness); !status)
            return reject(status);
        if (scratch.decoded_witness.count != canonical.count ||
            scratch.decoded_witness.targets != canonical.targets)
            return reject(Status::fault("regular move journal generated witness identity"));
        for (usize index = 0; index < canonical.count; ++index)
            if (scratch.witness_before[index] != canonical.before[index])
                return reject(Status::fault("regular move journal generated witness preimage"));
        store(output.descriptor, 1020, jbd2::detail::crc32c(seed, output.descriptor));
        store(output.commit, 52, update.seconds); store(output.commit, 56, update.nanoseconds);
        store(output.commit, 16, jbd2::detail::crc32c(seed, output.commit));
        store(output.journal_head, 24, output.sequence); store(output.journal_head, 28, 1);
        store(output.journal_head, 252, 0);
        store(output.journal_head, 252, jbd2::detail::crc32c(~u32{0}, output.journal_head));
        const auto new_codec = jbd2::Codec::open(output.journal_head, u32(binding.layout.journal_blocks),
            binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!new_codec) return reject(new_codec.status());
        std::array<jbd2::BlockRange, 5> allowed{};
        for (usize index = 0; index < output.count; ++index) allowed[index] = {output.targets[index], 1};
        Records records{output};
        const auto parsed = jbd2::build_replay_plan(new_codec.value(), records,
            {binding.layout.filesystem_blocks, std::span<const jbd2::BlockRange>{allowed}.first(output.count)},
            {scratch.record, scratch.payload, scratch.actions, scratch.revocations});
        if (!parsed) return reject(parsed.status());
        const auto &replay = parsed.value();
        if (replay.actions.size() != output.count || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.incomplete_tail || replay.empty_log ||
            replay.next_sequence != output.sequence + 1)
            return reject(Status::fault("regular move journal generated record shape"));
        for (usize index = 0; index < replay.actions.size(); ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != output.targets[index] || action.journal_block != index + 2 ||
                action.sequence != output.sequence || action.transaction_order)
                return reject(Status::fault("regular move journal generated target identity"));
            const auto &payload = output.payloads[index];
            for (usize byte = 0; byte < payload.size(); ++byte)
            {
                const u8 restored = action.restore_magic && byte < 4 ? u8(jbd2::magic >> (24 - byte * 8)) : payload[byte];
                if (restored != canonical.after[index][byte])
                    return reject(Status::fault("regular move journal generated payload differs"));
            }
        }
        output.ready = true;
        return {};
    }
};
} // namespace ok::fs
