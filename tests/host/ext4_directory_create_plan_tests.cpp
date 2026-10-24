#include "ext4_directory_create_fixture.hpp"
#include <ok/fs/ext4_directory_create_plan.hpp>
#include <ok/fs/ext4_directory_create_witness.hpp>

using namespace mkdir_plan_fixture;

namespace
{
struct DirectoryFixture : Fixture
{
    Block blocks{}, data{};
    u32 physical{69};
    explicit DirectoryFixture(bool shared = false) : Fixture(shared)
    {
        blocks.fill(0xff);
        u32 count{};
        for (u32 block = physical; count < 1000; ++block)
        {
            CHECK(block < binding.layout.filesystem_blocks);
            if (block == directory_block || (block >= 1000 && block < 2024)) continue;
            blocks[(block - 1) / 8] &= u8(~(1U << ((block - 1) % 8)));
            ++count;
        }
        refresh_blocks();
    }
    void refresh_blocks()
    {
        put16(descriptors, 24, u16(jbd2::detail::crc32c(seed, blocks)));
        refresh_descriptor();
    }
    Ext4CreateUpdate mkdir_update() const
    { return {parent, 1700000111, 123456789, "inbox", {NodeType::directory, 02750, 0x10001, 0x20002}}; }
    Status plan(Ext4CreatePlan &out) const
    {
        const auto inode_source = source();
        return Ext4DirectoryCreatePlanner::plan(binding, mkdir_update(),
            {inode_source, 0, physical, blocks, data}, out);
    }
};

void complete_directory(bool shared)
{
    DirectoryFixture fixture(shared);
    Ext4CreatePlan plan{}; CHECK(fixture.plan(plan));
    CHECK(plan.count == (shared ? 7 : 8));
    CHECK(plan.allocated_directory_block == 69 && plan.block_group == 0);
    CHECK(plan.targets[plan.count - 3] == 3 && plan.targets[plan.count - 2] == 69 &&
          plan.targets[plan.count - 1] == 100);
    const usize new_index = shared ? 0 : 1;
    const usize parent_offset = ((fixture.parent - 1) % 4) * 256;
    const usize new_offset = ((fixture.inode - 1) % 4) * 256;
    const auto parent = std::span<const u8>{plan.after[0]}.subspan(parent_offset, 256);
    const auto inode = std::span<const u8>{plan.after[new_index]}.subspan(new_offset, 256);
    using namespace ext4_checked;
    CHECK(le16(parent, 26) == 3 && le16(inode, 26) == 2);
    CHECK(le16(inode, 0) == 0042750 && le32(inode, 4) == 1024 && le32(inode, 28) == 2);
    CHECK((u32(le16(inode, 2)) | (u32(le16(inode, 120)) << 16)) == 0x10001);
    CHECK((u32(le16(inode, 24)) | (u32(le16(inode, 122)) << 16)) == 0x20002);
    CHECK(le16(inode, 42) == 1 && le16(inode, 56) == 1 && le32(inode, 60) == 69);
    CHECK(le32(inode, 8) == 1700000111 && le32(inode, 140) == (123456789U << 2));
    CHECK(inode_checksum(fixture.seed, fixture.inode, inode));
    CHECK(inode_checksum(fixture.seed, fixture.parent, parent));
    const auto &directory = plan.after[plan.count - 2];
    CHECK(le32(directory, 0) == fixture.inode && le16(directory, 4) == 12 &&
          directory[6] == 1 && directory[7] == 2 && directory[8] == '.');
    CHECK(le32(directory, 12) == fixture.parent && le16(directory, 16) == 1000 &&
          directory[18] == 2 && directory[19] == 2 && directory[20] == '.' && directory[21] == '.');
    for (usize at = 22; at < 1012; ++at) CHECK(directory[at] == 0);
    CHECK(le32(directory, 1012) == 0 && le16(directory, 1016) == 12 &&
          directory[1018] == 0 && directory[1019] == 0xde);
    CHECK(le32(directory, 1020) == jbd2::detail::crc32c(
        inode_seed(fixture.seed, fixture.inode, inode), std::span<const u8>{directory}.first(1012)));
    CHECK(le32(plan.after[new_index + 3], 12) == 999 && le16(plan.after[new_index + 2], 12) == 999 &&
          le16(plan.after[new_index + 2], 16) == 2);
    CHECK((plan.after[plan.count - 3][8] & (1U << 4)) != 0);
    usize found{};
    const auto &parent_directory = plan.after[plan.count - 1];
    for (usize at = 0; at < 1012; at += le16(parent_directory, at + 4))
        if (le32(parent_directory, at) == fixture.inode)
        {
            CHECK(parent_directory[at + 7] == 2 && parent_directory[at + 6] == 5);
            CHECK(std::string_view(reinterpret_cast<const char *>(parent_directory.data() + at + 8), 5) == "inbox");
            ++found;
        }
    CHECK(found == 1);
    Block descriptor{}; descriptor[12] = 0x55;
    CHECK(Ext4DirectoryCreateWitnessCodec::encode(fixture.mkdir_update(), plan, descriptor));
    CHECK(descriptor[12] == 0x55);
    std::array<Block, 8> before{};
    Ext4DirectoryCreateWitness witness{};
    CHECK(Ext4DirectoryCreateWitnessCodec::decode(descriptor, std::span<const Block>{plan.after}.first(plan.count),
          std::span<Block>{before}.first(plan.count), witness));
    CHECK(witness.inode == fixture.inode && witness.physical == 69 && witness.update.name == "inbox" &&
          witness.update.attributes.type == NodeType::directory);
    for (usize index = 0; index < plan.count; ++index) CHECK(before[index] == plan.before[index]);
    const auto unchanged = descriptor;
    CHECK(Ext4DirectoryCreateWitnessCodec::encode(witness.update, plan, descriptor));
    CHECK(descriptor == unchanged);
    for (usize offset : {160U, 168U, 172U, 204U, 271U, 279U, 1019U})
    {
        auto corrupted = descriptor; corrupted[offset] ^= 1;
        CHECK(!Ext4DirectoryCreateWitnessCodec::decode(corrupted,
            std::span<const Block>{plan.after}.first(plan.count),
            std::span<Block>{before}.first(plan.count), witness));
    }
    auto bad_padding = descriptor;
    bad_padding[44 + (plan.count - 1) * 16] = 1;
    CHECK(!Ext4DirectoryCreateWitnessCodec::decode(bad_padding,
        std::span<const Block>{plan.after}.first(plan.count),
        std::span<Block>{before}.first(plan.count), witness));
}

void refusals()
{
    Ext4CreatePlan out{};
    DirectoryFixture stale; stale.data[77] = 1;
    CHECK(stale.plan(out).code() == StatusCode::unsupported && !out.count);
    DirectoryFixture links;
    auto parent = std::span<u8>{links.parent_table}.subspan(((links.parent - 1) % 4) * 256, 256);
    put16(parent, 26, 0xffff); inode_crc(parent, links.parent, links.seed);
    CHECK(links.plan(out).code() == StatusCode::overflow && !out.count);
    DirectoryFixture checksum; checksum.blocks[8] ^= 1;
    CHECK(checksum.plan(out).code() == StatusCode::io_error && !out.count);
    DirectoryFixture reserved;
    reserved.blocks[0] &= u8(~1U); reserved.refresh_blocks();
    CHECK(reserved.plan(out).code() == StatusCode::io_error && !out.count);
    DirectoryFixture wrong; ++wrong.physical;
    CHECK(wrong.plan(out).code() == StatusCode::invalid_argument && !out.count);
    DirectoryFixture denied;
    auto denied_parent = std::span<u8>{denied.parent_table}.subspan(((denied.parent - 1) % 4) * 256, 256);
    put16(denied_parent, 0, 0040555); inode_crc(denied_parent, denied.parent, denied.seed);
    CHECK(denied.plan(out).code() == StatusCode::unsupported && !out.count);
    DirectoryFixture mode;
    auto update = mode.mkdir_update(); update.attributes.mode = 010000;
    const auto source = mode.source();
    CHECK(Ext4DirectoryCreatePlanner::plan(mode.binding, update,
        {source, 0, mode.physical, mode.blocks, mode.data}, out).code() == StatusCode::invalid_argument);
    Ext4CreatePlan oversized{}; oversized.count = 8; oversized.parent = 2;
    oversized.after[0].fill(1);
    Block descriptor{}; descriptor[12] = 0x55; const auto original = descriptor;
    CHECK(Ext4DirectoryCreateWitnessCodec::encode({2, 1, 0, "inbox", {NodeType::directory, 0755, 0, 0}},
        oversized, descriptor).code() == StatusCode::unsupported);
    CHECK(descriptor == original);
}
} // namespace

int main()
{
    complete_directory(false); complete_directory(true); refusals();
    std::puts("DIRECTORY_CREATE_PLAN_PASS seven/eight homes dot/dotdot links ownership counters checksums complete-witness refusals");
}
