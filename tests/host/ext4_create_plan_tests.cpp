#include <ok/fs/ext4_create_plan.hpp>
#include <ok/fs/ext4_create_witness.hpp>
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "create plan model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = Ext4CreateBlock;

void put16(std::span<u8> bytes, usize at, u16 value)
{ bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
void put32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (index * 8)); }
void inode_crc(std::span<u8> inode, u32 number, u32 seed)
{
    inode[124] = inode[125] = inode[130] = inode[131] = 0;
    const u32 value = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
    put16(inode, 124, u16(value)); put16(inode, 130, u16(value >> 16));
}

struct Fixture
{
    Ext4RecoveryInfo binding{};
    Block superblock{}, descriptors{}, bitmap{}, parent_table{}, new_table{}, directory{};
    u32 parent{}, inode{}, parent_block{}, new_block{}, directory_block{100}, seed{};

    explicit Fixture(bool same_table = false)
    {
        parent = same_table ? 11 : 2;
        inode = same_table ? 12 : 13;
        parent_block = same_table ? 7 : 5;
        new_block = same_table ? 7 : 8;
        binding.layout.block_size = 1024;
        binding.layout.filesystem_blocks = 8193;
        binding.layout.journal_first_block = 1000;
        binding.layout.journal_blocks = 1024;
        binding.metadata_count = 4;
        binding.metadata[0] = {1, 2};
        binding.metadata[1] = {3, 1};
        binding.metadata[2] = {4, 1};
        binding.metadata[3] = {5, 64};
        binding.journal_inode = 8;
        binding.needs_recovery = true;
        binding.empty_log = true;
        for (usize index = 0; index < binding.uuid.size(); ++index) binding.uuid[index] = u8(index + 1);
        seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        for (u32 number = 1; number < inode; ++number)
            bitmap[(number - 1) / 8] |= u8(1U << ((number - 1) % 8));

        put32(superblock, 0, 256); put32(superblock, 4, 8193);
        put32(superblock, 12, 1000); put32(superblock, 16, 257 - inode);
        put32(superblock, 20, 1); put32(superblock, 32, 8192);
        put32(superblock, 36, 8192); put32(superblock, 40, 256);
        put16(superblock, 56, 0xef53); put16(superblock, 58, 1);
        put32(superblock, 76, 1); put32(superblock, 84, 11);
        put16(superblock, 88, 256); put32(superblock, 92, 4);
        put32(superblock, 96, 0x46); put32(superblock, 100, 0x400);
        for (usize index = 0; index < binding.uuid.size(); ++index)
            superblock[104 + index] = binding.uuid[index];
        put32(superblock, 224, 8); put16(superblock, 254, 32);
        superblock[373] = 1;
        refresh_super();

        put32(descriptors, 0, 3); put32(descriptors, 4, 4); put32(descriptors, 8, 5);
        put16(descriptors, 12, 1000); put16(descriptors, 14, u16(257 - inode));
        put16(descriptors, 16, 1); put16(descriptors, 18, 4);
        put16(descriptors, 28, u16(257 - inode));
        refresh_descriptor();

        auto node = std::span<u8>{parent_table}.subspan(((parent - 1) % 4) * 256, 256);
        put16(node, 0, 0040755); put32(node, 4, 1024); put16(node, 26, 2);
        put32(node, 28, 2); put32(node, 32, 0x80000);
        put16(node, 40, 0xf30a); put16(node, 42, 1); put16(node, 44, 4);
        put16(node, 56, 1); put32(node, 60, directory_block);
        put16(node, 128, 32); inode_crc(node, parent, seed);
        if (same_table) new_table = parent_table;

        put32(directory, 0, parent); put16(directory, 4, 12);
        directory[6] = 1; directory[7] = 2; directory[8] = '.';
        put32(directory, 12, parent); put16(directory, 16, 12);
        directory[18] = 2; directory[19] = 2; directory[20] = '.'; directory[21] = '.';
        put32(directory, 24, 3); put16(directory, 28, 988);
        directory[30] = 4; directory[31] = 1;
        directory[32] = 's'; directory[33] = 'e'; directory[34] = 'e'; directory[35] = 'd';
        put16(directory, 1016, 12); directory[1019] = 0xde;
        refresh_directory();
    }

    void refresh_super()
    { put32(superblock, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{superblock}.first(1020))); }
    void refresh_descriptor()
    {
        put16(descriptors, 26, u16(jbd2::detail::crc32c(seed, std::span<const u8>{bitmap}.first(32))));
        put16(descriptors, 30, 0);
        const u16 checksum = u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(seed, ext4_checked::encoded(0)),
            std::span<const u8>{descriptors}.first(32)));
        put16(descriptors, 30, checksum);
    }
    void refresh_directory()
    {
        const auto node = std::span<const u8>{parent_table}.subspan(((parent - 1) % 4) * 256, 256);
        put32(directory, 1020, jbd2::detail::crc32c(
            ext4_checked::inode_seed(seed, parent, node), std::span<const u8>{directory}.first(1012)));
    }
    [[nodiscard]] Ext4CreateSource source() const
    { return {inode, 0, parent_block, new_block, directory_block,
              superblock, descriptors, bitmap, parent_table, new_table, directory}; }
    [[nodiscard]] Ext4CreateUpdate update(std::string_view name = "note") const
    { return {parent, 1700000111, 123456789, name, {NodeType::regular, 0640, 1000, 1001}}; }
};

void success_and_integrity()
{
    Fixture fixture;
    const auto selected = Ext4CreatePlanner::select_inode(
        fixture.binding, fixture.superblock, fixture.descriptors, fixture.bitmap, 0);
    CHECK(selected && selected.value() == 13);
    Ext4CreatePlan plan{};
    CHECK(Ext4CreatePlanner::plan(fixture.binding, fixture.update(), fixture.source(), plan));
    CHECK(plan.count == 6 && plan.inode == 13 && plan.parent == 2 && plan.directory_block == 100);
    CHECK(plan.targets[0] == 5 && plan.targets[1] == 8 && plan.targets[2] == 4 &&
          plan.targets[3] == 2 && plan.targets[4] == 1 && plan.targets[5] == 100);
    CHECK(plan.before[0] == fixture.parent_table && plan.before[1] == fixture.new_table &&
          plan.before[2] == fixture.bitmap && plan.before[3] == fixture.descriptors &&
          plan.before[4] == fixture.superblock && plan.before[5] == fixture.directory);
    for (usize offset = 0; offset < 1024; ++offset)
    {
        if (offset < 256 || offset >= 512) CHECK(plan.after[0][offset] == plan.before[0][offset]);
        if (offset >= 256) CHECK(plan.after[1][offset] == plan.before[1][offset]);
        if (offset != 1) CHECK(plan.after[2][offset] == plan.before[2][offset]);
        if ((offset < 14 || offset > 15) && (offset < 26 || offset > 31))
            CHECK(plan.after[3][offset] == plan.before[3][offset]);
        if ((offset < 16 || offset > 19) && offset < 1020)
            CHECK(plan.after[4][offset] == plan.before[4][offset]);
    }
    CHECK(plan.after[2][1] == u8(plan.before[2][1] | 0x10));
    CHECK(ext4_checked::le16(plan.after[3], 14) == 243);
    CHECK(ext4_checked::le16(plan.after[3], 28) == 243);
    CHECK(ext4_checked::le32(plan.after[4], 16) == 243);
    const auto created = std::span<const u8>{plan.after[1]}.first(256);
    CHECK(ext4_checked::le16(created, 0) == 0100640);
    CHECK(ext4_checked::le16(created, 2) == 1000 && ext4_checked::le16(created, 24) == 1001);
    CHECK(ext4_checked::le32(created, 4) == 0 && ext4_checked::le32(created, 28) == 0);
    CHECK(ext4_checked::le16(created, 26) == 1 && ext4_checked::le32(created, 32) == 0x80000);
    CHECK(ext4_checked::le16(created, 40) == 0xf30a && ext4_checked::le16(created, 42) == 0);
    CHECK(ext4_checked::inode_checksum(fixture.seed, 13, created));
    CHECK(ext4_checked::inode_checksum(fixture.seed, 2,
          std::span<const u8>{plan.after[0]}.subspan(256, 256)));
    CHECK(ext4_checked::le32(plan.after[5], 36) == 13);
    CHECK(ext4_checked::le16(plan.after[5], 40) == 976);
    CHECK(plan.after[5][42] == 4 && plan.after[5][43] == 1);
    CHECK(plan.after[5][44] == 'n' && plan.after[5][45] == 'o' &&
          plan.after[5][46] == 't' && plan.after[5][47] == 'e');
    CHECK(jbd2::detail::crc32c(ext4_checked::inode_seed(
          fixture.seed, 2, std::span<const u8>{plan.after[0]}.subspan(256, 256)),
          std::span<const u8>{plan.after[5]}.first(1012)) ==
          ext4_checked::le32(plan.after[5], 1020));
    CHECK(fixture.bitmap == plan.before[2] && fixture.directory == plan.before[5]);
}

void dedup_and_rejects()
{
    Fixture shared(true);
    Ext4CreatePlan plan{};
    CHECK(Ext4CreatePlanner::plan(shared.binding, shared.update(), shared.source(), plan));
    CHECK(plan.count == 5 && plan.targets[0] == 7 && plan.targets[1] == 4 &&
          plan.targets[2] == 2 && plan.targets[3] == 1 && plan.targets[4] == 100);
    CHECK(ext4_checked::inode_checksum(shared.seed, 11,
          std::span<const u8>{plan.after[0]}.subspan(512, 256)));
    CHECK(ext4_checked::inode_checksum(shared.seed, 12,
          std::span<const u8>{plan.after[0]}.subspan(768, 256)));

    Fixture original;
    auto reject = [&](Fixture &fixture, Ext4CreateUpdate update, StatusCode code) {
        Ext4CreatePlan output{};
        const auto result = Ext4CreatePlanner::plan(fixture.binding, update, fixture.source(), output);
        CHECK(!result && result.code() == code);
    };
    reject(original, original.update("seed"), StatusCode::already_exists);
    auto wrong_type = original.update(); wrong_type.attributes.type = NodeType::directory;
    reject(original, wrong_type, StatusCode::unsupported);
    auto special = original.update(); special.attributes.mode = 04755;
    reject(original, special, StatusCode::invalid_argument);
    auto bad_clock = original.update(); bad_clock.nanoseconds = 1000000000;
    reject(original, bad_clock, StatusCode::invalid_argument);
    reject(original, original.update("bad/name"), StatusCode::invalid_argument);

    Fixture bad_bitmap;
    bad_bitmap.bitmap[1] ^= 0x10;
    reject(bad_bitmap, bad_bitmap.update(), StatusCode::io_error);
    Fixture bad_super;
    bad_super.superblock[1020] ^= 1;
    reject(bad_super, bad_super.update(), StatusCode::io_error);
    Fixture bad_descriptor;
    bad_descriptor.descriptors[30] ^= 1;
    reject(bad_descriptor, bad_descriptor.update(), StatusCode::io_error);
    Fixture bad_counter;
    put16(bad_counter.descriptors, 14, 1); bad_counter.refresh_descriptor();
    reject(bad_counter, bad_counter.update(), StatusCode::io_error);
    Fixture bad_inode;
    bad_inode.new_table[0] = 1;
    reject(bad_inode, bad_inode.update(), StatusCode::unsupported);
    Fixture bad_parent;
    bad_parent.parent_table[256 + 124] ^= 1;
    reject(bad_parent, bad_parent.update(), StatusCode::io_error);
    Fixture bad_tail;
    bad_tail.directory[1020] ^= 1;
    reject(bad_tail, bad_tail.update(), StatusCode::io_error);
    Fixture dirty_slack;
    dirty_slack.directory[36] = 0xa5; dirty_slack.refresh_directory();
    reject(dirty_slack, dirty_slack.update(), StatusCode::no_space);
    Fixture uninitialized;
    put16(uninitialized.descriptors, 18, 5); uninitialized.refresh_descriptor();
    reject(uninitialized, uninitialized.update(), StatusCode::io_error);
    Fixture exhausted;
    for (auto &byte : exhausted.bitmap) byte = 0xff;
    put16(exhausted.descriptors, 14, 0); put16(exhausted.descriptors, 28, 0);
    put32(exhausted.superblock, 16, 0); exhausted.refresh_super(); exhausted.refresh_descriptor();
    CHECK(Ext4CreatePlanner::select_inode(exhausted.binding, exhausted.superblock,
          exhausted.descriptors, exhausted.bitmap, 0).status().code() == StatusCode::no_space);
    exhausted.bitmap[0] ^= 1;
    CHECK(Ext4CreatePlanner::select_inode(exhausted.binding, exhausted.superblock,
          exhausted.descriptors, exhausted.bitmap, 0).status().code() == StatusCode::io_error);
    Ext4CreatePlan output{};
    auto aliased = original.source(); aliased.directory_block = 4;
    CHECK(Ext4CreatePlanner::plan(original.binding, original.update(), aliased, output).code() ==
          StatusCode::unsupported);
    auto wrong_table = original.source(); wrong_table.new_table_block = 4;
    CHECK(Ext4CreatePlanner::plan(original.binding, original.update(), wrong_table, output).code() ==
          StatusCode::unsupported);
}

void exact_witness()
{
    for (bool shared : {false, true}) for (u32 generation : {0U, 7U})
    {
        Fixture fixture(shared);
        if (generation)
        {
            auto record = std::span<u8>{fixture.new_table}.subspan(((fixture.inode - 1) % 4) * 256, 256);
            CHECK(Ext4FreeInode::encode(record, fixture.inode, fixture.seed, generation));
            if (shared) fixture.parent_table = fixture.new_table;
        }
        Ext4CreatePlan plan{};
        CHECK(Ext4CreatePlanner::plan(fixture.binding, fixture.update(), fixture.source(), plan));
        Block descriptor{};
        CHECK(Ext4CreateWitnessCodec::encode(fixture.update(), plan, descriptor));
        std::array<Block, 6> before{};
        Ext4CreateWitness decoded{};
        CHECK(Ext4CreateWitnessCodec::decode(descriptor, std::span<const Block>{plan.after}.first(plan.count),
              std::span<Block>{before}.first(plan.count), decoded));
        CHECK(decoded.count == plan.count && decoded.inode == fixture.inode && decoded.update.name == "note");
        for (usize index = 0; index < plan.count; ++index) CHECK(before[index] == plan.before[index]);
        const auto changed = [&](usize offset) {
            auto bad = descriptor; bad[offset] ^= 1;
            Ext4CreateWitness rejected{};
            CHECK(!Ext4CreateWitnessCodec::decode(bad, std::span<const Block>{plan.after}.first(plan.count),
                std::span<Block>{before}.first(plan.count), rejected));
        };
        changed(128); changed(136); changed(140); changed(172); changed(231); changed(1019);
    }
    // Exact preimages are mandatory even if a hypothetical large plan does not
    // fit; failure leaves the descriptor unchanged for its caller to discard.
    Ext4CreatePlan oversized{}; oversized.count = 6; oversized.parent = 2;
    for (usize i = 0; i < 1024; ++i) oversized.after[0][i] = 1;
    Block descriptor{}; descriptor[12] = 0x55; const auto original = descriptor;
    const Ext4CreateUpdate update{2, 1, 0, "note", {NodeType::regular, 0600, 1000, 1000}};
    CHECK(Ext4CreateWitnessCodec::encode(update, oversized, descriptor).code() == StatusCode::unsupported);
    CHECK(descriptor == original);
}
} // namespace

int main()
{
    success_and_integrity();
    dedup_and_rejects();
    exact_witness();
    std::puts("PASS bounded empty-create image plan: selection, six/five homes, checksums, exact deltas and rejects");
}
