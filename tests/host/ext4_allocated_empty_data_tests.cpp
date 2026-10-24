#include <ok/fs/ext4_allocated_empty_data_plan.hpp>
#include "ext4_zero_truncate_test_support.hpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
using namespace zero_fixture;
namespace
{
usize positive{}, refusal{};
Ext4AllocatedEmptyDataScratch scratch_storage{};
Ext4AllocatedEmptyDataPlan result_storage{};
auto *scratch = &scratch_storage;
auto *result = &result_storage;
Ext4FirstDataPlan fresh_storage{};

Ext4AllocatedEmptyDataSource source(const Fixture &f)
{ return {f.group, f.physical, f.table_block, f.superblock, f.descriptors, f.bitmap, f.inode_bitmap, f.inode_table}; }
Ext4AllocatedEmptyDataUpdate update(const Fixture &f, u32 size = 73)
{ return {f.inode, 9, size, 1700000222, 987654321}; }
std::span<u8> node(Fixture &f)
{ return std::span<u8>{f.inode_table}.subspan(((f.inode - 1) % 4) * 256, 256); }
void refresh(Fixture &f)
{ stamp_inode(node(f), f.inode, f.seed); }
void select_slot(Fixture &f, u32 slot)
{
    std::array<u8, 256> old{};
    for (usize at = 0; at < 256; ++at) old[at] = f.inode_table[at];
    for (usize at = 0; at < f.inode_table.size(); ++at) f.inode_table[at] = u8((at * 17 + 19) % 251);
    f.inode += slot;
    for (usize at = 0; at < 256; ++at) node(f)[at] = old[at];
    f.inode_bitmap.fill(0); f.inode_bitmap[(f.inode - 1) / 8] |= u8(1U << ((f.inode - 1) % 8));
    put16(f.descriptors, 26, u16(jbd2::detail::crc32c(f.seed, std::span<const u8>{f.inode_bitmap}.first(32))));
    f.refresh_descriptor_crc(0); refresh(f);
}
Status plan(const Fixture &f, const Ext4AllocatedEmptyDataUpdate &request)
{ return Ext4AllocatedEmptyDataPlanner::plan_off_stack(f.binding, request, source(f), *scratch, *result); }
void unchanged(const Fixture &f, const Fixture &original)
{
    CHECK(f.superblock == original.superblock && f.descriptors == original.descriptors &&
        f.bitmap == original.bitmap && f.other_bitmap == original.other_bitmap &&
        f.inode_bitmap == original.inode_bitmap && f.inode_table == original.inode_table);
}
void verify(const Fixture &f, const Ext4AllocatedEmptyDataUpdate &request, u64 version)
{
    using namespace ext4_checked;
    CHECK(result->count == 4 && result->update.inode == f.inode && result->update.generation == 9);
    CHECK(result->physical == f.physical && result->group == f.group);
    CHECK(result->old_version == version && result->new_version == version + 1);
    CHECK((result->targets == std::array<u32, 4>{f.group ? 8195U : 3U, 2, 1, f.table_block}));
    CHECK((result->before == std::array<Block, 4>{f.bitmap, f.descriptors, f.superblock, f.inode_table}));
    const u32 local = (f.physical - 1) % 8192;
    for (usize at = 0; at < 1024; ++at)
        CHECK(result->after[0][at] == (at == local / 8 ? u8(f.bitmap[at] | (1U << (local % 8))) : f.bitmap[at]));
    auto group = std::span<const u8>{result->after[1]}.subspan(f.group * 32, 32);
    CHECK(le16(group, 12) == 0 && le32(result->after[2], 12) == 0);
    CHECK(u16(jbd2::detail::crc32c(f.seed, result->after[0])) == le16(group, 24));
    auto checked_descriptor = std::array<u8, 32>{};
    for (usize at = 0; at < 32; ++at) checked_descriptor[at] = group[at];
    checked_descriptor[30] = checked_descriptor[31] = 0;
    CHECK(u16(jbd2::detail::crc32c(jbd2::detail::crc32c(f.seed, encoded(f.group)), checked_descriptor)) == le16(group, 30));
    for (usize at = 0; at < 1024; ++at)
    {
        const bool changed_group = at >= f.group * 32 && at < f.group * 32 + 32 &&
            (at % 32 == 12 || at % 32 == 13 || at % 32 == 24 || at % 32 == 25 || at % 32 >= 30);
        if (!changed_group) CHECK(result->after[1][at] == f.descriptors[at]);
        if (!(at >= 12 && at < 16) && at < 1020) CHECK(result->after[2][at] == f.superblock[at]);
    }
    CHECK(jbd2::detail::crc32c(~u32{0}, std::span<const u8>{result->after[2]}.first(1020)) == le32(result->after[2], 1020));
    const usize offset = ((f.inode - 1) % 4) * 256;
    auto final = std::span<const u8>{result->after[3]}.subspan(offset, 256);
    CHECK(le32(final, 4) == request.size && le32(final, 28) == 2);
    CHECK(le32(final, 12) == request.seconds && le32(final, 16) == request.seconds);
    CHECK(le32(final, 132) == request.nanoseconds << 2 && le32(final, 136) == request.nanoseconds << 2);
    CHECK((u64{le32(final, 36)} | u64{le32(final, 152)} << 32) == version + 1);
    CHECK(le16(final, 42) == 1 && le32(final, 52) == 0 && le16(final, 56) == 1 &&
        le16(final, 58) == 0 && le32(final, 60) == f.physical);
    CHECK(inode_checksum(f.seed, f.inode, final));
    for (usize at = 0; at < 1024; ++at)
    {
        if (at < offset || at >= offset + 256) CHECK(result->after[3][at] == f.inode_table[at]);
        else
        {
            const usize inside = at - offset;
            const bool permitted = (inside >= 4 && inside < 8) || (inside >= 12 && inside < 20) ||
                (inside >= 28 && inside < 40) || (inside >= 42 && inside < 44) ||
                (inside >= 56 && inside < 64) || (inside >= 124 && inside < 126) ||
                (inside >= 130 && inside < 140) || (inside >= 152 && inside < 156);
            if (!permitted) CHECK(result->after[3][at] == f.inode_table[at]);
        }
    }
}
void positives()
{
    constexpr u64 versions[]{0, 1, 0xfffffffeULL, 0xffffffffULL, 0x100000000ULL,
        0x7fffffffeULL, ~u64{0} - 1};
    for (bool cross : {false, true}) for (u32 slot = 0; slot < 4; ++slot)
        for (u32 size : {1U, 2U, 1023U, 1024U}) for (u64 version : versions)
        {
            Fixture f(cross); select_slot(f, slot); auto inode = node(f);
            put32(inode, 36, u32(version)); put32(inode, 152, u32(version >> 32));
            put32(inode, 8, 123); put32(inode, 12, 456); put32(inode, 16, 789); put32(inode, 144, 42);
            put32(inode, 140, 4); put32(inode, 132, 8); put32(inode, 136, 12); put32(inode, 148, 16);
            refresh(f); const auto old = f; const auto request = update(f, size);
            CHECK(plan(f, request)); verify(f, request, version); unchanged(f, old); ++positive;
            CHECK(plan(f, request)); verify(f, request, version); unchanged(f, old); ++positive;
        }
    for (bool cross : {false, true})
    {
        Fixture f(cross); put32(node(f), 36, 0); put32(node(f), 152, 0); refresh(f);
        const auto request = update(f); CHECK(plan(f, request));
        const Ext4FirstDataSource fresh_source{f.group, f.physical, f.table_block, f.superblock, f.descriptors, f.bitmap, f.inode_table};
        auto *fresh = &fresh_storage;
        CHECK(Ext4FirstDataPlanner::plan(f.binding, {f.inode, request.size, request.seconds, request.nanoseconds}, fresh_source, *fresh));
        CHECK(result->before == fresh->before && result->after == fresh->after && result->targets == fresh->targets); ++positive;
    }
}
void refusals()
{
    for (bool cross : {false, true})
    {
        Fixture f(cross); const auto original = f;
        for (u32 field = 0; field < 8; ++field)
        {
            auto request = update(f);
            if (field == 0) request.inode = 10;
            if (field == 1) ++request.generation;
            if (field == 2) request.size = 0;
            if (field == 3) request.size = 1025;
            if (field == 4) request.seconds = 0x80000000U;
            if (field == 5) request.nanoseconds = 1000000000U;
            if (field == 6) request.inode = 0;
            if (field == 7) request.inode = cross ? 513 : 257;
            result->count = 99; CHECK(!plan(f, request)); CHECK(result->count == 0); unchanged(f, original); ++refusal;
        }
        for (usize at : {usize{20}, usize{28}, usize{32}, usize{42}, usize{46}, usize{48}, usize{52},
                         usize{104}, usize{108}, usize{112}, usize{116}, usize{118}, usize{126}, usize{128}, usize{156}, usize{160}, usize{255}})
        {
            Fixture altered = f; node(altered)[at] ^= 1; refresh(altered);
            CHECK(!plan(altered, update(altered))); CHECK(result->count == 0); ++refusal;
        }
        Fixture max = f; put32(node(max), 36, ~u32{0}); put32(node(max), 152, ~u32{0}); refresh(max);
        CHECK(plan(max, update(max)).code() == StatusCode::overflow); CHECK(result->count == 0); ++refusal;
        Fixture nonempty = f; put32(node(nonempty), 4, 1); refresh(nonempty);
        CHECK(!plan(nonempty, update(nonempty))); CHECK(result->count == 0); ++refusal;
        Fixture missing = f; missing.inode_bitmap.fill(0);
        put16(missing.descriptors, 26, u16(jbd2::detail::crc32c(missing.seed, std::span<const u8>{missing.inode_bitmap}.first(32))));
        missing.refresh_descriptor_crc(0); CHECK(!plan(missing, update(missing))); CHECK(result->count == 0); ++refusal;
        Fixture stale = f; stale.inode_table[124] ^= 1;
        CHECK(!plan(stale, update(stale))); CHECK(result->count == 0); ++refusal;
        auto bad_source = source(f); ++bad_source.physical;
        CHECK(!Ext4AllocatedEmptyDataPlanner::plan_off_stack(f.binding, update(f), bad_source, *scratch, *result)); ++refusal;
        const auto request = update(f);
        scratch->prepared.count = 88;
        CHECK(!Ext4AllocatedEmptyDataPlanner::plan_off_stack(f.binding, request, source(f), *scratch, scratch->prepared));
        CHECK(scratch->prepared.count == 88); ++refusal;
    }
}
} // namespace
int main()
{
    positives(); refusals();
    std::printf("PASS hosted allocated-empty data planner positive=%zu refusal=%zu\n", positive, refusal);
    return 0;
}
