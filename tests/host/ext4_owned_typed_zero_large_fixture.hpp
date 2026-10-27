#pragma once
// Author-adapted exact inherited large zero-truncate synthetic fixture.
// Original SHA256: ee87a2dfb79e45552a8978081090429bfe5a2222f6a44dbc6bf504398357bb2c
#include <ok/fs/ext4/txn.hpp>
#include <cstdio>
#include <cstdlib>
namespace typed_owner_zero_large_fixture {
using namespace ok; using namespace ok::fs;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "zero owner fixture:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
using Block = Ext4ZeroTruncateBlock;
using Registry = block::BlockSessions<1>;
using Session = Registry::Session;
using Preview = Ext4ZeroTruncateSessionPreview<Session>;
constexpr u32 groups = 32, blocks_per_group = 8192, inodes_per_group = 256;
constexpr u32 filesystem_blocks = 1 + groups * blocks_per_group;
constexpr u32 root_first = 3000, root_blocks = 1024, journal_first = 1000;
constexpr u32 target_number = 13, target_first = 72, target_blocks = 16;
constexpr u32 multi_number = 14, last_admitted = 136, extra_number = 137;
constexpr u64 session_origin = 11, volume_origin = 4;
constexpr u32 table_first = 5, extra_table = table_first + (extra_number - 1) / 4;

void put16(std::span<u8> bytes, usize at, u16 value)
{ bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
void put32(std::span<u8> bytes, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) bytes[at + byte] = u8(value >> (byte * 8)); }
void be32(std::span<u8> bytes, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) bytes[at + byte] = u8(value >> (24 - byte * 8)); }
void stamp_inode(std::span<u8> bytes, u32 number, u32 seed)
{
    put16(bytes, 124, 0); put16(bytes, 130, 0);
    const u32 checksum = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, bytes), bytes);
    put16(bytes, 124, u16(checksum)); put16(bytes, 130, u16(checksum >> 16));
}
void stamp_descriptor(std::span<u8> bytes, u32 group, u32 seed)
{
    put16(bytes, 30, 0);
    put16(bytes, 30, u16(jbd2::detail::crc32c(
        jbd2::detail::crc32c(seed, ext4_checked::encoded(group)), bytes)));
}

struct Image
{
    Ext4RecoveryInfo binding{};
    // Retain only structural bitmaps and canonical participant images. Table,
    // directory, and remaining logical blocks are generated on demand.
    std::array<Block, groups> block_bitmaps{}, inode_bitmaps{};
    Block superblock{}, descriptors{}, target_table{}, old_head{}, first_directory{}, empty_directory{};
    Ext4ZeroTruncatePlan planning{}, plan{};
    Ext4ZeroTruncateJournalFormatScratch formatting{};
    Ext4ZeroTruncateJournalFormat journal{};
    u32 seed{}, mask{}, allocated_records{};
    bool extra{};

    static u32 begin(u32 group) { return 1 + group * blocks_per_group; }
    static u32 cross_physical(u32 extent) { return begin(extent + 1) + 70; }
    bool allocated(u32 number) const
    {
        return number == 2 || number == 8 ||
            (number >= 11 && number <= last_admitted) || (extra && number == extra_number);
    }
    void allocate_block(u32 physical)
    {
        const u32 group = (physical - 1) / blocks_per_group;
        const u32 local = (physical - 1) % blocks_per_group;
        block_bitmaps[group][local / 8] |= u8(1U << (local % 8));
    }
    void inode(u32 number, std::span<u8, 256> out) const
    {
        for (auto &byte : out) byte = 0;
        if (!allocated(number)) return;
        put16(out, 0, number == 2 ? 0040755 : 0100640);
        put16(out, 26, number == 2 ? 2 : 1);
        put32(out, 32, 0x80000); put32(out, 100, 9); put32(out, 36, 42);
        put16(out, 40, 0xf30a); put16(out, 44, 4); put16(out, 128, 32);
        const auto extent = [&](u32 index, u32 logical, u32 length, u32 physical)
        {
            const usize at = 52 + usize(index) * 12;
            put32(out, at, logical); put16(out, at + 4, u16(length)); put32(out, at + 8, physical);
        };
        if (number == 2 || number == 8)
        {
            put32(out, 4, root_blocks * 1024); put32(out, 28, root_blocks * 2);
            put16(out, 42, 1); extent(0, 0, root_blocks, number == 2 ? root_first : journal_first);
        }
        else if (number == target_number)
        {
            put32(out, 4, (target_blocks - 1) * 1024 + 1); put32(out, 28, target_blocks * 2);
            put16(out, 42, 1); extent(0, 0, target_blocks, target_first);
        }
        else if (number == multi_number)
        {
            put32(out, 4, 4096); put32(out, 28, 8); put16(out, 42, 4);
            for (u32 index = 0; index < 4; ++index) extent(index, index, 1, cross_physical(index));
        }
        stamp_inode(out, number, seed);
    }
    void table(u32 group, u32 local, Block &out) const
    {
        for (u32 slot = 0; slot < 4; ++slot)
            inode(group * inodes_per_group + (local - 4) * 4 + slot + 1,
                std::span<u8, 256>{out.data() + slot * 256, 256});
    }
    void directory(Block &out, bool dots) const
    {
        out = {};
        if (dots)
        {
            put32(out, 0, 2); put16(out, 4, 12); out[6] = 1; out[7] = 2; out[8] = '.';
            put32(out, 12, 2); put16(out, 16, 1000); out[18] = 2; out[19] = 2;
            out[20] = out[21] = '.';
        }
        else put16(out, 4, 1012);
        put16(out, 1016, 12); out[1019] = 0xde;
        std::array<u8, 256> root{}; inode(2, root);
        put32(out, 1020, jbd2::detail::crc32c(
            ext4_checked::inode_seed(seed, 2, root), std::span<const u8>{out}.first(1012)));
    }
    void initialize(bool add_extra)
    {
        binding = {}; superblock = {}; descriptors = {}; old_head = {}; extra = add_extra; mask = 0;
        block_bitmaps.fill(Block{}); inode_bitmaps.fill(Block{}); allocated_records = 0;
        binding.layout = {volume_origin, filesystem_blocks, journal_first, 1024, 1024};
        binding.metadata_count = groups * 4; binding.journal_inode = 8; binding.journal_sequence = 70;
        binding.needs_recovery = true; binding.empty_log = true;
        for (usize byte = 0; byte < binding.uuid.size(); ++byte) binding.uuid[byte] = u8(byte + 1);
        seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        for (u32 group = 0; group < groups; ++group)
        {
            const u32 first = begin(group);
            binding.metadata[group * 4] = {first, 2};
            binding.metadata[group * 4 + 1] = {first + 2, 1};
            binding.metadata[group * 4 + 2] = {first + 3, 1};
            binding.metadata[group * 4 + 3] = {first + 4, 64};
            for (u32 local = 0; local < 68; ++local) allocate_block(first + local);
        }
        for (u32 number = 1; number <= groups * inodes_per_group; ++number)
            if (allocated(number))
            {
                const u32 group = (number - 1) / inodes_per_group;
                const u32 local = (number - 1) % inodes_per_group;
                inode_bitmaps[group][local / 8] |= u8(1U << (local % 8)); ++allocated_records;
            }
        CHECK(allocated_records == (extra ? 129U : 128U));
        for (u32 block = 0; block < root_blocks; ++block)
        { allocate_block(root_first + block); allocate_block(journal_first + block); }
        for (u32 block = 0; block < target_blocks; ++block) allocate_block(target_first + block);
        for (u32 index = 0; index < 4; ++index) allocate_block(cross_physical(index));
        u32 total_free_blocks = 0, total_free_inodes = 0;
        for (u32 group = 0; group < groups; ++group)
        {
            const u32 first = begin(group);
            auto d = std::span<u8>{descriptors}.subspan(group * 32, 32);
            put32(d, 0, first + 2); put32(d, 4, first + 3); put32(d, 8, first + 4); put16(d, 18, 4);
            u32 free_blocks = 0, free_inodes = 0;
            for (u32 bit = 0; bit < blocks_per_group; ++bit)
                if (!(block_bitmaps[group][bit / 8] & (1U << (bit % 8)))) ++free_blocks;
            for (u32 bit = 0; bit < inodes_per_group; ++bit)
                if (!(inode_bitmaps[group][bit / 8] & (1U << (bit % 8)))) ++free_inodes;
            total_free_blocks += free_blocks; total_free_inodes += free_inodes;
            put16(d, 12, u16(free_blocks)); put16(d, 14, u16(free_inodes)); put16(d, 16, group ? 0 : 1);
            put16(d, 24, u16(jbd2::detail::crc32c(seed, block_bitmaps[group])));
            put16(d, 26, u16(jbd2::detail::crc32c(seed,
                std::span<const u8>{inode_bitmaps[group]}.first(inodes_per_group / 8))));
            stamp_descriptor(d, group, seed);
        }
        put32(superblock, 0, groups * inodes_per_group); put32(superblock, 4, filesystem_blocks);
        put32(superblock, 12, total_free_blocks); put32(superblock, 16, total_free_inodes); put32(superblock, 20, 1);
        put32(superblock, 32, blocks_per_group); put32(superblock, 36, blocks_per_group);
        put32(superblock, 40, inodes_per_group); put16(superblock, 56, 0xef53); put16(superblock, 58, 1);
        put32(superblock, 76, 1); put32(superblock, 84, 11); put16(superblock, 88, 256);
        put32(superblock, 92, 4); put32(superblock, 96, 0x46); put32(superblock, 100, 0x400);
        for (usize byte = 0; byte < binding.uuid.size(); ++byte) superblock[104 + byte] = binding.uuid[byte];
        put32(superblock, 224, 8); put16(superblock, 254, 32); superblock[373] = 1;
        put32(superblock, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{superblock}.first(1020)));
        table(0, 7, target_table); directory(first_directory, true); directory(empty_directory, false);
        const Ext4ZeroTruncateSource source{0, 8, superblock, descriptors, block_bitmaps[0], inode_bitmaps[0], target_table};
        const Ext4ZeroTruncateUpdate update{target_number, 9, (target_blocks - 1) * 1024 + 1, 1700000111, 765432100};
        CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(binding, update, source, planning, plan));
        CHECK(plan.released_count == target_blocks && plan.group == 0);
        be32(old_head, 0, jbd2::magic); be32(old_head, 4, u32(jbd2::RecordType::superblock_v2));
        be32(old_head, 12, 1024); be32(old_head, 16, 1024); be32(old_head, 20, 1);
        be32(old_head, 24, binding.journal_sequence); be32(old_head, 40, jbd2::feature_checksum_v3);
        for (usize byte = 0; byte < binding.uuid.size(); ++byte) old_head[48 + byte] = binding.uuid[byte];
        be32(old_head, 64, 1); old_head[80] = 4;
        be32(old_head, 252, jbd2::detail::crc32c(~u32{0}, old_head));
        CHECK(Ext4ZeroTruncateJournalFormatter::format(binding, plan, old_head, inode_bitmaps[0], formatting, journal));
        CHECK(journal.ready); binding.empty_log = false; binding.journal_sequence = journal.active_sequence;
        if (extra)
        {
            // The refusing 129th record is independently a valid canonical
            // allocated-empty inode, in a table block unused by records 1–128.
            std::array<u8, 256> bytes{}; inode(extra_number, bytes);
            std::array<u32, 16> mapping{}; u32 count{};
            CHECK(ext4_checked::inode_checksum(seed, extra_number, bytes));
            CHECK(Ext4ZeroTruncatePlanner::inspect_inode(binding, extra_number, bytes, mapping, count));
            CHECK(!count);
        }
    }
    void render(u32 number, Block &out) const
    {
        for (usize home = 0; home < 4; ++home)
            if (number == plan.targets[home])
            { out = mask & (1U << home) ? plan.after[home] : plan.before[home]; return; }
        if (number >= journal_first && number < journal_first + 8)
        {
            const u32 index = number - journal_first;
            if (!index) out = journal.head;
            else if (index == 1) out = journal.descriptor;
            else if (index < 6) out = journal.payloads[index - 2];
            else out = index == 6 ? journal.commit : journal.end;
            return;
        }
        out = {};
        if (!number) return;
        const u32 group = (number - 1) / blocks_per_group, local = (number - 1) % blocks_per_group;
        if (local == 2) out = block_bitmaps[group];
        else if (local == 3) out = inode_bitmaps[group];
        else if (local >= 4 && local < 68) table(group, local, out);
        else if (number >= root_first && number < root_first + root_blocks)
            out = number == root_first ? first_directory : empty_directory;
    }
};

#undef CHECK
} // namespace typed_owner_zero_large_fixture
