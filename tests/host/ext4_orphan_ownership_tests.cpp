#include "ok/fs/ext4/alloc.hpp"
#include "ext4_orphan_access_fixture.hpp"

namespace
{
struct Tables
{
    std::array<std::vector<Block>, 4> groups{};

    Tables(const Image &image, const Fixture &fixture)
    {
        for (u32 group = 0; group < 4; ++group)
        {
            if (ext4_checked::le16(fixture.descriptors, group * 32 + 18) & 1) continue;
            const u32 first = value(fixture.descriptors, group * 32 + 8);
            groups[group].reserve(512);
            for (u32 offset = 0; offset < 512; ++offset)
                groups[group].push_back(load(image, first + offset));
        }
        for (usize index = 0; index < fixture.count; ++index)
        {
            const auto &entry = fixture.images[index];
            const u32 group = (entry.number - 1) / 2048;
            const u32 first = value(fixture.descriptors, group * 32 + 8);
            CHECK(entry.table_block >= first && entry.table_block < first + 512);
            groups[group][entry.table_block - first] = entry.table;
        }
    }
    Ext4OrphanOwnershipSource source(const Fixture &fixture) const
    {
        std::array<std::span<const Block>, 4> spans{};
        for (usize group = 0; group < groups.size(); ++group)
            spans[group] = groups[group];
        return {fixture.source(), spans};
    }
    Block &inode_table(const Fixture &fixture, u32 number)
    {
        const u32 group = (number - 1) / 2048;
        const u32 first = value(fixture.descriptors, group * 32 + 8);
        const u32 block = first + ((number - 1) % 2048) / 4;
        return groups[group][block - first];
    }
};

void reject(const Ext4RecoveryInfo &binding, const Fixture &fixture,
            const Tables &tables, StatusCode expected, usize &count)
{
    Ext4OrphanOwnershipScratch scratch{};
    Ext4OrphanOwnershipProof proof{};
    const auto status = Ext4OrphanOwnershipValidator::validate(
        binding, tables.source(fixture), scratch, proof);
    CHECK(!status && status.code() == expected && !proof.valid &&
          proof.allocated_inodes == 0 && proof.owned_blocks == 0);
    ++count;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    std::ifstream filled_input(argv[1], std::ios::binary);
    CHECK(filled_input);
    const Image filled_image((std::istreambuf_iterator<char>(filled_input)), {});
    CHECK(filled_image.size() == 32 * 1024 * 1024);
    MemoryDisk filled_disk(filled_image);
    Ext4RecoveryBinding filled_admission;
    CHECK(filled_admission.inspect(filled_disk, {0, filled_image.size() / 512}));
    CHECK(filled_admission.info());
    const auto &filled_binding = *filled_admission.info();
    const usize filled_reads = filled_disk.reads;
    Fixture filled(filled_image, filled_binding);
    filled.add(filled_image, 12, 14);
    filled.add(filled_image, 14, 0);
    filled.head(12);
    Tables filled_tables(filled_image, filled);
    Ext4OrphanOwnershipScratch scratch{};
    Ext4OrphanOwnershipProof proof{};
    CHECK(Ext4OrphanOwnershipValidator::validate(
        filled_binding, filled_tables.source(filled), scratch, proof));
    CHECK(proof.valid && proof.filesystem_uuid == filled_binding.uuid &&
          proof.observed_head == 12 && proof.orphan_count == 2 &&
          proof.allocated_inodes == 16 && proof.ordinary_ranges == 8 &&
          proof.owned_blocks == 80);
    CHECK(filled_disk.reads == filled_reads && !filled_disk.writes && !filled_disk.flushes);

    std::ifstream empty_input(argv[2], std::ios::binary);
    CHECK(empty_input);
    const Image empty_image((std::istreambuf_iterator<char>(empty_input)), {});
    CHECK(empty_image.size() == filled_image.size());
    MemoryDisk empty_disk(empty_image);
    Ext4RecoveryBinding empty_admission;
    CHECK(empty_admission.inspect(empty_disk, {0, empty_image.size() / 512}));
    CHECK(empty_admission.info());
    const auto &empty_binding = *empty_admission.info();
    const usize empty_reads = empty_disk.reads;
    Fixture empty(empty_image, empty_binding);
    empty.add(empty_image, 14, 0);
    empty.head(14);
    Tables empty_tables(empty_image, empty);
    CHECK(Ext4OrphanOwnershipValidator::validate(
        empty_binding, empty_tables.source(empty), scratch, proof));
    CHECK(proof.valid && proof.observed_head == 14 && proof.orphan_count == 1 &&
          proof.allocated_inodes == 15 && proof.ordinary_ranges == 6 &&
          proof.owned_blocks == 78);
    CHECK(empty_disk.reads == empty_reads && !empty_disk.writes && !empty_disk.flushes);

    usize rejected = 0;
    auto missing_table = filled_tables;
    missing_table.groups[0].pop_back();
    reject(filled_binding, filled, missing_table, StatusCode::invalid_argument, rejected);
    auto changed_chain_table = filled_tables;
    changed_chain_table.inode_table(filled, 12)[slot(12) + 4] ^= 1;
    reject(filled_binding, filled, changed_chain_table, StatusCode::io_error, rejected);
    auto shared = filled;
    const u32 shared_block = value(filled.images[0].table, slot(12) + 60);
    for (usize index = 0; index < shared.count; ++index)
        if (shared.images[index].table_block == shared.table_for(15))
        {
            store32(std::span<u8>{shared.images[index].table}, slot(15) + 60, shared_block);
            shared.stamp_inode(shared.images[index].table, 15);
        }
    Tables shared_tables(filled_image, shared);
    reject(filled_binding, shared, shared_tables, StatusCode::io_error, rejected);
    auto metadata_alias = filled;
    for (usize index = 0; index < metadata_alias.count; ++index)
        if (metadata_alias.images[index].table_block == metadata_alias.table_for(15))
        {
            store32(std::span<u8>{metadata_alias.images[index].table}, slot(15) + 60, 5);
            metadata_alias.stamp_inode(metadata_alias.images[index].table, 15);
        }
    Tables metadata_tables(filled_image, metadata_alias);
    reject(filled_binding, metadata_alias, metadata_tables, StatusCode::io_error, rejected);
    auto journal_alias = filled;
    for (usize index = 0; index < journal_alias.count; ++index)
        if (journal_alias.images[index].table_block == journal_alias.table_for(15))
        {
            store32(std::span<u8>{journal_alias.images[index].table}, slot(15) + 60,
                    u32(filled_binding.layout.journal_first_block));
            journal_alias.stamp_inode(journal_alias.images[index].table, 15);
        }
    Tables journal_tables(filled_image, journal_alias);
    reject(filled_binding, journal_alias, journal_tables, StatusCode::io_error, rejected);
    auto opaque_reserved = filled_tables;
    opaque_reserved.inode_table(filled, 1)[slot(1) + 40] = 1;
    reject(filled_binding, filled, opaque_reserved, StatusCode::unsupported, rejected);
    auto wrong_journal = filled_tables;
    store32(std::span<u8>{wrong_journal.inode_table(filled, 8)}, slot(8) + 60, shared_block);
    filled.stamp_inode(wrong_journal.inode_table(filled, 8), 8);
    reject(filled_binding, filled, wrong_journal, StatusCode::unsupported, rejected);
    auto extent_tree = filled;
    for (usize index = 0; index < extent_tree.count; ++index)
        if (extent_tree.images[index].table_block == extent_tree.table_for(15))
        {
            store16(std::span<u8>{extent_tree.images[index].table}, slot(15) + 46, 1);
            extent_tree.stamp_inode(extent_tree.images[index].table, 15);
        }
    Tables tree_tables(filled_image, extent_tree);
    reject(filled_binding, extent_tree, tree_tables, StatusCode::unsupported, rejected);
    auto hidden = filled;
    hidden.inode_bitmaps[2][0] = 1;
    Tables hidden_tables(filled_image, hidden);
    reject(filled_binding, hidden, hidden_tables, StatusCode::unsupported, rejected);
    auto leaked = filled;
    constexpr u32 unowned = 597;
    const u32 local = unowned - 1;
    CHECK(!(leaked.block_bitmaps[0][local / 8] & (1U << (local % 8))));
    leaked.block_bitmaps[0][local / 8] |= u8(1U << (local % 8));
    leaked.stamp_block_bitmap(0);
    store16(std::span<u8>{leaked.descriptors}, 12,
            u16(ext4_checked::le16(leaked.descriptors, 12) - 1));
    leaked.stamp_descriptor(0);
    store32(std::span<u8>{leaked.super}, 12, value(leaked.super, 12) - 1);
    leaked.stamp_super();
    Tables leaked_tables(filled_image, leaked);
    reject(filled_binding, leaked, leaked_tables, StatusCode::io_error, rejected);
    auto unallocated_data = filled;
    const u32 nonorphan_block = value(filled.images[1].table, slot(15) + 60);
    const u32 nonorphan_local = nonorphan_block - 1;
    unallocated_data.block_bitmaps[0][nonorphan_local / 8] &=
        u8(~(1U << (nonorphan_local % 8)));
    unallocated_data.stamp_block_bitmap(0);
    store16(std::span<u8>{unallocated_data.descriptors}, 12,
            u16(ext4_checked::le16(unallocated_data.descriptors, 12) + 1));
    unallocated_data.stamp_descriptor(0);
    store32(std::span<u8>{unallocated_data.super}, 12,
            value(unallocated_data.super, 12) + 1);
    unallocated_data.stamp_super();
    Tables unallocated_tables(filled_image, unallocated_data);
    reject(filled_binding, unallocated_data, unallocated_tables, StatusCode::io_error, rejected);
    CHECK(rejected == 11 && filled_disk.reads == filled_reads &&
          !filled_disk.writes && !filled_disk.flushes);
    std::printf("PASS orphan global ownership: filled, empty, allocated=%zu, rejects=%zu no_device_io=1\n",
                proof.allocated_inodes, rejected);
}
