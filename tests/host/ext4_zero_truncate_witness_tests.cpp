#include "ext4_zero_truncate_test_support.hpp"
#include <ok/fs/ext4_zero_truncate_witness.hpp>
#include <cstdio>
#include <cstdlib>

using namespace zero_fixture;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "zero truncate witness model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
Ext4ZeroTruncatePlan planner{}, plan{}, decoded{};
Ext4ZeroTruncateWitnessScratch workspace{};
Ext4ZeroTruncateWitness witness{};
Block descriptor{};
usize positive{}, refusal{}, mixtures{};
constexpr u32 sequence = 71;

void be32(Block &bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> ((3 - index) * 8)); }
void stamp_descriptor(Block &bytes, u32 seed)
{ be32(bytes, 1020, 0); be32(bytes, 1020, jbd2::detail::crc32c(seed, bytes)); }
void descriptor_tags(const Fixture &f, const Ext4ZeroTruncatePlan &p, Block &bytes)
{
    bytes = {};
    be32(bytes, 0, jbd2::magic); be32(bytes, 4, u32(jbd2::RecordType::descriptor));
    be32(bytes, 8, sequence);
    for (usize home = 0; home < 4; ++home)
    {
        const usize at = home ? 44 + (home - 1) * 16 : 12;
        const bool escaped = jbd2::detail::be32(p.after[home], 0) == jbd2::magic;
        const u32 flags = (home ? jbd2::flag_same_uuid : 0) |
            (home == 3 ? jbd2::flag_last_tag : 0) | (escaped ? jbd2::flag_escape : 0);
        be32(bytes, at, p.targets[home]); be32(bytes, at + 4, flags);
        Block payload = p.after[home];
        if (escaped) be32(payload, 0, 0);
        be32(bytes, at + 12, jbd2::detail::crc32c(jbd2::detail::crc32c(f.seed,
            std::span<const u8>{bytes}.subspan(8, 4)), payload));
    }
}
void mapped(Fixture &f, u32 blocks = 3, u32 records = 2, bool partial = true)
{
    auto inode = std::span<u8>{f.inode_table}.first(256);
    put32(inode, 4, partial ? (blocks - 1) * 1024 + 19 : blocks * 1024);
    put32(inode, 28, blocks * 2);
    records = records < blocks ? records : blocks;
    put16(inode, 42, u16(records));
    u32 logical{};
    for (u32 record = 0; record < records; ++record)
    {
        const u32 count = (blocks - logical + records - record - 1) / (records - record);
        const usize at = 52 + record * 12;
        put32(inode, at, logical); put16(inode, at + 4, u16(count));
        put32(inode, at + 8, f.physical + 2 + logical + record * 3);
        logical += count;
    }
    put32(inode, 8, 1699999000); put32(inode, 140, 17U << 2);
    put32(inode, 144, 1600000000); put32(inode, 148, 39U << 2);
    f.refresh_inode();
    for (usize at = 256; at < 1024; ++at) f.inode_table[at] = u8(at * 17);
}
Ext4ZeroTruncateUpdate update(const Fixture &f)
{
    const usize offset = ((f.inode - 1) % 4) * 256;
    return {f.inode, 9, ext4_checked::le32(f.inode_table, offset + 4), 1700000111, 765432100};
}
void move_target(Fixture &f, u32 local_slot)
{
    std::array<u8, 256> inode{};
    for (usize index = 0; index < 256; ++index) inode[index] = f.inode_table[index];
    f.inode = 13 + local_slot;
    for (usize index = 0; index < 256; ++index) f.inode_table[local_slot * 256 + index] = inode[index];
    stamp_inode(std::span<u8>{f.inode_table}.subspan(local_slot * 256, 256), f.inode, f.seed);
    be32(f.inode_table, 0, jbd2::magic);
    f.inode_bitmap[1] = u8(1U << ((f.inode - 1) % 8));
    put16(f.descriptors, 26, u16(jbd2::detail::crc32c(f.seed,
        std::span<const u8>{f.inode_bitmap}.first(32))));
    f.refresh_descriptor_crc(0);
}
void smaller_geometry(Fixture &f)
{
    f.binding.layout.filesystem_blocks = 2049;
    f.binding.metadata[3].count = 32;
    put32(f.superblock, 0, 128); put32(f.superblock, 4, 2049);
    put32(f.superblock, 32, 2048); put32(f.superblock, 36, 2048);
    put32(f.superblock, 40, 128); put32(f.superblock, 16, 127);
    put16(f.descriptors, 14, 127);
    put16(f.descriptors, 24, u16(jbd2::detail::crc32c(f.seed,
        std::span<const u8>{f.bitmap}.first(256))));
    put16(f.descriptors, 26, u16(jbd2::detail::crc32c(f.seed,
        std::span<const u8>{f.inode_bitmap}.first(16))));
    f.refresh_descriptor_crc(0); f.refresh_super();
}
void prepare(Fixture &f)
{
    const auto request = update(f);
    CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(f.binding, request, f.source(), planner, plan));
    descriptor_tags(f, plan, descriptor);
    CHECK(Ext4ZeroTruncateWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
        sequence, workspace, descriptor));
    stamp_descriptor(descriptor, f.seed);
}
void decode_ok(const Fixture &f)
{
    CHECK(Ext4ZeroTruncateWitnessCodec::decode(f.binding, descriptor, plan.after,
        f.inode_bitmap, workspace, decoded, witness));
    CHECK(decoded.before == plan.before && decoded.after == plan.after && decoded.targets == plan.targets);
    CHECK(decoded.released_mapping == plan.released_mapping && decoded.count == 4 && !decoded.noop);
    CHECK(witness.update.inode == f.inode && witness.update.generation == 9 &&
        witness.update.old_size == update(f).old_size && witness.update.seconds == update(f).seconds &&
        witness.update.nanoseconds == update(f).nanoseconds && witness.uuid == f.binding.uuid &&
        witness.sequence == sequence && witness.group == f.group &&
        witness.targets == plan.targets && witness.released_count == plan.released_count &&
        witness.released_mapping == plan.released_mapping);
    for (usize index = 0; index < 256; ++index)
        CHECK(descriptor[Ext4ZeroTruncateWitnessCodec::inode_begin + index] == f.inode_table[((f.inode - 1) % 4) * 256 + index]);
    for (usize at = Ext4ZeroTruncateWitnessCodec::length; at < Ext4ZeroTruncateWitnessCodec::end; ++at)
        CHECK(!descriptor[at]);
    ++positive;
}
void decode_refused(const Fixture &f)
{
    const auto before_descriptor = descriptor;
    const auto before_after = plan.after;
    const auto before_bitmap = f.inode_bitmap;
    CHECK(!Ext4ZeroTruncateWitnessCodec::decode(f.binding, descriptor, plan.after,
        f.inode_bitmap, workspace, decoded, witness));
    CHECK(decoded.count == 0 && !witness.sequence && !witness.released_count);
    CHECK(descriptor == before_descriptor && plan.after == before_after && f.inode_bitmap == before_bitmap);
    ++refusal;
}
void mutations()
{
    // Re-stamped corruption cannot bypass fixed lengths, identity, exact
    // inverse transforms, original-inode checksum or canonical regeneration.
    for (usize at : {usize{128}, usize{136}, usize{138}, usize{140}, usize{141}, usize{142},
            usize{144}, usize{160}, usize{164}, usize{168}, usize{172}, usize{176}, usize{180},
            usize{184}, usize{188}, usize{204}, usize{220}, usize{236}, usize{248}, usize{300},
            usize{304}, usize{308}, usize{404}, usize{428}, usize{559}, usize{560}, usize{1019},
            usize{28}, usize{92}, usize{12}, usize{16}, usize{20}, usize{24}, usize{84}, usize{88}})
    {
        Fixture f; mapped(f); prepare(f);
        descriptor[at] ^= 1; stamp_descriptor(descriptor, f.seed); decode_refused(f);
    }
    for (usize at : {usize{0}, usize{4}, usize{8}, usize{1020}})
    {
        Fixture f; mapped(f); prepare(f); descriptor[at] ^= 1; decode_refused(f);
    }
    for (u32 count : {0U, 17U, 255U})
    {
        Fixture f; mapped(f); prepare(f); descriptor[141] = u8(count);
        stamp_descriptor(descriptor, f.seed); decode_refused(f);
    }
    for (usize home = 0; home < 4; ++home)
        for (usize at : {usize{0}, usize{127}, usize{256}, usize{1023}})
        {
            Fixture f; mapped(f); prepare(f); plan.after[home][at] ^= 1; decode_refused(f);
        }
    for (usize home = 0; home < 4; ++home)
        for (bool after : {false, true})
        {
            Fixture f; mapped(f); prepare(f);
            (after ? plan.after : plan.before)[home][700] ^= 1;
            descriptor_tags(f, plan, descriptor);
            const auto old = descriptor;
            CHECK(!Ext4ZeroTruncateWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
                sequence, workspace, descriptor));
            CHECK(descriptor == old); ++refusal;
        }
    {
        Fixture f; mapped(f); prepare(f); ++plan.released_mapping[0];
        descriptor_tags(f, plan, descriptor); const auto old = descriptor;
        CHECK(!Ext4ZeroTruncateWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
            sequence, workspace, descriptor)); CHECK(descriptor == old); ++refusal;
    }
    for (u32 field : {0U, 1U, 2U, 3U, 4U})
    {
        Fixture f; mapped(f); prepare(f);
        if (field == 0) ++plan.generation;
        if (field == 1) ++plan.old_size;
        if (field == 2) ++plan.group;
        if (field == 3) plan.count = 3;
        if (field == 4) plan.noop = true;
        descriptor_tags(f, plan, descriptor); const auto old = descriptor;
        CHECK(!Ext4ZeroTruncateWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
            sequence, workspace, descriptor)); CHECK(descriptor == old); ++refusal;
    }
    for (u32 bad_sequence : {0U, sequence + 1})
    {
        Fixture f; mapped(f); prepare(f); const auto old = descriptor;
        CHECK(!Ext4ZeroTruncateWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
            bad_sequence, workspace, descriptor)); CHECK(descriptor == old); ++refusal;
    }
    for (usize at : {usize{0}, usize{4}, usize{16}, usize{20}, usize{28}, usize{92}})
    {
        Fixture f; mapped(f); prepare(f); descriptor[at] ^= 1; const auto old = descriptor;
        CHECK(!Ext4ZeroTruncateWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
            sequence, workspace, descriptor)); CHECK(descriptor == old); ++refusal;
    }
    {
        Fixture f; mapped(f); prepare(f); f.inode_bitmap[1] ^= 1; decode_refused(f);
    }
    {
        Fixture f; mapped(f); prepare(f); f.binding.uuid[0] ^= 1; decode_refused(f);
    }
    {
        Fixture f; mapped(f); prepare(f); ++f.binding.metadata[3].first; decode_refused(f);
    }
    // A valid original-inode checksum and CRC bindings do not permit replacing
    // retained atime/birthtime, owner identity or generation in the after inode.
    for (usize at : {usize{2}, usize{8}, usize{24}, usize{26}, usize{100}, usize{140}, usize{144}, usize{148}})
    {
        Fixture f; mapped(f); prepare(f);
        plan.after[0][at] ^= 1;
        stamp_inode(std::span<u8>{plan.after[0]}.first(256), f.inode, f.seed);
        Block tags{}; descriptor_tags(f, plan, tags);
        for (usize index = 0; index < 92; ++index) descriptor[index] = tags[index];
        put32(descriptor, 220, jbd2::detail::crc32c(~u32{0}, plan.after[0]));
        stamp_descriptor(descriptor, f.seed); decode_refused(f);
    }
    // Alias refusal precedes any clearing or copying of source memory.
    {
        Fixture f; mapped(f); prepare(f);
        const auto old = workspace.reconstructed;
        CHECK(!Ext4ZeroTruncateWitnessCodec::decode(f.binding, descriptor, plan.after,
            f.inode_bitmap, workspace, workspace.reconstructed, witness));
        CHECK(workspace.reconstructed.before == old.before && workspace.reconstructed.after == old.after); ++refusal;
        const auto old_descriptor = descriptor;
        CHECK(!Ext4ZeroTruncateWitnessCodec::encode(f.binding, plan, descriptor,
            sequence, workspace, descriptor)); CHECK(descriptor == old_descriptor); ++refusal;
        const auto old_after = plan.after;
        CHECK(!Ext4ZeroTruncateWitnessCodec::decode(f.binding, descriptor, plan.after,
            f.inode_bitmap, workspace, plan, witness)); CHECK(plan.after == old_after); ++refusal;
        const auto old_scratch = workspace.block;
        CHECK(!Ext4ZeroTruncateWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
            sequence, workspace, workspace.block)); CHECK(workspace.block == old_scratch); ++refusal;
    }
    {
        Fixture f;
        const auto request = update(f);
        CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(f.binding, request, f.source(), planner, plan));
        CHECK(plan.noop && !plan.count);
        descriptor = {}; be32(descriptor, 0, jbd2::magic);
        be32(descriptor, 4, u32(jbd2::RecordType::descriptor)); be32(descriptor, 8, sequence);
        const auto old = descriptor;
        CHECK(!Ext4ZeroTruncateWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
            sequence, workspace, descriptor)); CHECK(descriptor == old); ++refusal;
    }
}
void home_mixtures(const Fixture &f)
{
    decode_ok(f);
    for (u32 mask = 0; mask < 16; ++mask)
    {
        std::array<Block, 4> homes{};
        for (usize home = 0; home < 4; ++home) homes[home] = (mask & (1U << home)) ? decoded.after[home] : decoded.before[home];
        u8 result = 0xff;
        CHECK(Ext4ZeroTruncateHomeMatcher::classify(decoded, homes, result)); CHECK(result == mask); ++mixtures;
        for (usize home = 0; home < 4; ++home)
        {
            homes[home][700] ^= 1; result = 0xff;
            CHECK(!Ext4ZeroTruncateHomeMatcher::classify(decoded, homes, result)); CHECK(!result);
            homes[home][700] ^= 1; ++refusal;
        }
    }
    {
        const auto old = decoded.before[0];
        CHECK(!Ext4ZeroTruncateHomeMatcher::classify(decoded, decoded.before, decoded.before[0][700]));
        CHECK(decoded.before[0] == old); ++refusal;
    }
}
} // namespace

int main()
{
    for (bool cross : {false, true})
        for (u32 blocks = 1; blocks <= 16; ++blocks)
            for (u32 records = 1; records <= 4; ++records)
                for (bool partial : {false, true})
                {
                    Fixture f(cross); mapped(f, blocks, records, partial); prepare(f); decode_ok(f);
                }
    for (bool cross : {false, true})
    {
        Fixture f(cross); mapped(f, 16, 4); prepare(f); home_mixtures(f);
    }
    for (u32 slot = 1; slot < 4; ++slot)
    {
        Fixture f; mapped(f, 16, 4); move_target(f, slot); prepare(f); decode_ok(f);
        CHECK(jbd2::detail::be32(plan.after[0], 0) == jbd2::magic);
        CHECK(jbd2::detail::be32(descriptor, 16) & jbd2::flag_escape);
    }
    {
        Fixture f; mapped(f, 16, 4); smaller_geometry(f); prepare(f); decode_ok(f);
    }
    {
        Fixture f; f.physical = 2046; mapped(f, 1, 1); smaller_geometry(f); prepare(f); decode_ok(f);
        CHECK(plan.released_mapping[0] == 2048);
    }
    {
        Fixture f(true); f.physical = 8702; mapped(f, 1, 1); prepare(f); decode_ok(f);
        CHECK(plan.released_mapping[0] == 8704);
    }
    mutations();
    std::printf("PASS hosted zero-truncate witness positive=%zu refusal=%zu mixtures=%zu\n", positive, refusal, mixtures);
}
