#include "ext4_zero_truncate_test_support.hpp"
#include <ok/fs/ext4/extent.hpp>
#include <cstdio>
#include <cstdlib>

using namespace zero_fixture;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "allocated empty witness:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
namespace
{
Ext4AllocatedEmptyDataScratch planning{};
Ext4AllocatedEmptyDataPlan plan{}, decoded{}, altered{};
Ext4AllocatedEmptyDataWitnessScratch workspace{};
Ext4AllocatedEmptyDataWitness witness{};
Block descriptor{}, saved_descriptor{}, mutated{};
std::array<Block, 4> homes{};
usize positive{}, refusal{}, mixtures{}, repaired_descriptor{}, repaired_after{};
constexpr u32 sequence = 71;
void be32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
void stamp(Block &bytes, u32 seed)
{ be32(bytes, 1020, 0); be32(bytes, 1020, jbd2::detail::crc32c(seed, bytes)); }
std::span<u8> node(Fixture &f)
{ return std::span<u8>{f.inode_table}.subspan(((f.inode - 1) % 4) * 256, 256); }
void relocate(Fixture &f, u32 slot)
{
    std::array<u8, 256> old{};
    for (usize at = 0; at < 256; ++at) old[at] = f.inode_table[at];
    for (usize at = 0; at < 1024; ++at) f.inode_table[at] = u8((at * 19 + 7) % 251);
    f.inode += slot;
    for (usize at = 0; at < 256; ++at) node(f)[at] = old[at];
    f.inode_bitmap.fill(0); f.inode_bitmap[(f.inode - 1) / 8] |= u8(1U << ((f.inode - 1) % 8));
    put16(f.descriptors, 26, u16(jbd2::detail::crc32c(f.seed, std::span<const u8>{f.inode_bitmap}.first(32))));
    f.refresh_descriptor_crc(0);
    if (slot) be32(f.inode_table, 0, jbd2::magic); // Actual JBD2 payload escaping.
    stamp_inode(node(f), f.inode, f.seed);
}
void tags(const Fixture &f, const Ext4AllocatedEmptyDataPlan &p, Block &bytes)
{
    bytes = {};
    be32(bytes, 0, jbd2::magic); be32(bytes, 4, u32(jbd2::RecordType::descriptor)); be32(bytes, 8, sequence);
    for (usize home = 0; home < 4; ++home)
    {
        const usize at = home ? 44 + (home - 1) * 16 : 12;
        const bool escaped = jbd2::detail::be32(p.after[home], 0) == jbd2::magic;
        const u32 flags = (home ? jbd2::flag_same_uuid : 0) |
            (home == 3 ? jbd2::flag_last_tag : 0) | (escaped ? jbd2::flag_escape : 0);
        be32(bytes, at, p.targets[home]); be32(bytes, at + 4, flags);
        mutated = p.after[home]; if (escaped) be32(mutated, 0, 0);
        be32(bytes, at + 12, jbd2::detail::crc32c(jbd2::detail::crc32c(f.seed,
            std::span<const u8>{bytes}.subspan(8, 4)), mutated));
    }
}
void prepare(Fixture &f, u32 size = 73)
{
    const Ext4AllocatedEmptyDataUpdate update{f.inode, 9, size, 1700000222, 987654321};
    const Ext4AllocatedEmptyDataSource source{f.group, f.physical, f.table_block,
        f.superblock, f.descriptors, f.bitmap, f.inode_bitmap, f.inode_table};
    CHECK(Ext4AllocatedEmptyDataPlanner::plan_off_stack(f.binding, update, source, planning, plan));
    tags(f, plan, descriptor);
    CHECK(Ext4AllocatedEmptyDataWitnessCodec::encode(f.binding, plan, f.inode_bitmap, sequence, workspace, descriptor));
    stamp(descriptor, f.seed);
}
void success(const Fixture &f)
{
    CHECK(Ext4AllocatedEmptyDataWitnessCodec::decode(f.binding, descriptor, plan.after,
        f.inode_bitmap, workspace, decoded, witness));
    CHECK(decoded.before == plan.before && decoded.after == plan.after && decoded.targets == plan.targets && decoded.count == 4);
    CHECK(decoded.old_version == plan.old_version && decoded.new_version == plan.new_version);
    CHECK(witness.update.inode == f.inode && witness.update.generation == 9 && witness.update.size == plan.update.size &&
        witness.update.seconds == plan.update.seconds && witness.update.nanoseconds == plan.update.nanoseconds &&
        witness.physical == f.physical && witness.group == f.group && witness.sequence == sequence &&
        witness.uuid == f.binding.uuid && witness.targets == plan.targets &&
        witness.old_version == plan.old_version && witness.new_version == plan.new_version);
    const usize offset = ((f.inode - 1) % 4) * 256;
    for (usize at = 0; at < 256; ++at)
        CHECK(descriptor[Ext4AllocatedEmptyDataWitnessCodec::inode_begin + at] == f.inode_table[offset + at]);
    ++positive;
}
void rejected(const Fixture &f, std::span<const Block, 4> after)
{
    saved_descriptor = descriptor;
    CHECK(!Ext4AllocatedEmptyDataWitnessCodec::decode(f.binding, descriptor, after,
        f.inode_bitmap, workspace, decoded, witness));
    CHECK(decoded.count == 0 && witness.sequence == 0 && witness.update.inode == 0 && witness.new_version == 0);
    CHECK(descriptor == saved_descriptor); ++refusal;
}
void positives()
{
    for (bool cross : {false, true}) for (u32 slot = 0; slot < 4; ++slot)
        for (u32 size : {1U, 1023U, 1024U})
            for (u64 version : {u64{0}, u64{1}, u64{0xffffffff}, u64{0x100000000}, ~u64{0} - 1})
            {
                Fixture f(cross); relocate(f, slot);
                put32(node(f), 36, u32(version)); put32(node(f), 152, u32(version >> 32));
                stamp_inode(node(f), f.inode, f.seed); prepare(f, size); success(f);
                const auto frozen = descriptor;
                CHECK(Ext4AllocatedEmptyDataWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
                    sequence, workspace, descriptor));
                CHECK(descriptor == frozen); success(f);
                for (u8 mask = 0; mask < 16; ++mask)
                {
                    for (usize home = 0; home < 4; ++home)
                        homes[home] = mask & (1U << home) ? plan.after[home] : plan.before[home];
                    u8 classified = 99;
                    CHECK(Ext4AllocatedEmptyDataHomeMatcher::classify(decoded, homes, classified));
                    CHECK(classified == mask); ++mixtures;
                }
            }
}
void corruptions()
{
    for (bool cross : {false, true})
    {
        Fixture f(cross); relocate(f, 3); prepare(f); const auto original = descriptor;
        for (usize at = 0; at < 1020; ++at)
        {
            descriptor = original; descriptor[at] ^= 1; stamp(descriptor, f.seed);
            rejected(f, plan.after); ++repaired_descriptor;
        }
        descriptor = original;
        for (usize home = 0; home < 4; ++home) for (usize at = 0; at < 1024; ++at)
        {
            homes = plan.after; homes[home][at] ^= 1; descriptor = original;
            put32(descriptor, 224 + home * 4, jbd2::detail::crc32c(~u32{0}, homes[home]));
            const usize tag = home ? 44 + (home - 1) * 16 : 12;
            const bool escaped = jbd2::detail::be32(homes[home], 0) == jbd2::magic;
            be32(descriptor, tag + 4, (home ? jbd2::flag_same_uuid : 0) |
                (home == 3 ? jbd2::flag_last_tag : 0) | (escaped ? jbd2::flag_escape : 0));
            mutated = homes[home]; if (escaped) be32(mutated, 0, 0);
            be32(descriptor, tag + 12, jbd2::detail::crc32c(jbd2::detail::crc32c(f.seed,
                std::span<const u8>{descriptor}.subspan(8, 4)), mutated));
            stamp(descriptor, f.seed); rejected(f, homes); ++repaired_after;
        }
        descriptor = original; descriptor[1023] ^= 1; rejected(f, plan.after);
        descriptor = original;
        for (usize home = 0; home < 4; ++home) for (usize at : {usize{0}, usize{17}, usize{1023}})
        {
            altered = plan; altered.before[home][at] ^= 1; saved_descriptor = descriptor;
            CHECK(!Ext4AllocatedEmptyDataWitnessCodec::encode(f.binding, altered, f.inode_bitmap,
                sequence, workspace, descriptor)); CHECK(descriptor == saved_descriptor); ++refusal;
            altered = plan; altered.after[home][at] ^= 1;
            CHECK(!Ext4AllocatedEmptyDataWitnessCodec::encode(f.binding, altered, f.inode_bitmap,
                sequence, workspace, descriptor)); CHECK(descriptor == saved_descriptor); ++refusal;
        }
        altered = plan; ++altered.old_version; saved_descriptor = descriptor;
        CHECK(!Ext4AllocatedEmptyDataWitnessCodec::encode(f.binding, altered, f.inode_bitmap,
            sequence, workspace, descriptor)); CHECK(descriptor == saved_descriptor); ++refusal;
        for (u8 mask = 0; mask < 16; ++mask) for (usize home = 0; home < 4; ++home)
        {
            for (usize part = 0; part < 4; ++part)
                homes[part] = mask & (1U << part) ? plan.after[part] : plan.before[part];
            homes[home][17] ^= 1; u8 classified = 99;
            CHECK(!Ext4AllocatedEmptyDataHomeMatcher::classify(plan, homes, classified));
            CHECK(classified == 0); ++refusal;
        }
        // Missing inode allocation is refused even with a repaired bitmap CRC.
        const auto original_bitmap = f.inode_bitmap;
        f.inode_bitmap.fill(0); rejected(f, plan.after); f.inode_bitmap = original_bitmap;
        workspace.encoded.fill(19); const auto alias_before = workspace.encoded;
        CHECK(!Ext4AllocatedEmptyDataWitnessCodec::encode(f.binding, plan, f.inode_bitmap,
            sequence, workspace, workspace.encoded)); CHECK(workspace.encoded == alias_before); ++refusal;
        workspace.reconstructed.count = 99;
        CHECK(!Ext4AllocatedEmptyDataWitnessCodec::decode(f.binding, descriptor, plan.after,
            f.inode_bitmap, workspace, workspace.reconstructed, witness));
        CHECK(workspace.reconstructed.count == 99); ++refusal;
        decoded = plan;
        CHECK(!Ext4AllocatedEmptyDataWitnessCodec::decode(f.binding, descriptor, decoded.after,
            f.inode_bitmap, workspace, decoded, witness)); CHECK(decoded.count == 4); ++refusal;
        homes = plan.after; const auto alias_homes = homes;
        CHECK(!Ext4AllocatedEmptyDataHomeMatcher::classify(plan, homes, homes[1][2]));
        CHECK(homes == alias_homes); ++refusal;
    }
}
} // namespace
int main()
{
    positives(); corruptions();
    std::printf("PASS hosted allocated-empty witness positive=%zu refusal=%zu mixtures=%zu repaired_descriptor=%zu repaired_after=%zu\n",
        positive, refusal, mixtures, repaired_descriptor, repaired_after);
    return 0;
}
