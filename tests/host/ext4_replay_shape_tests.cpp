#include <ok/fs/ext4/recovery.hpp>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace ok;
using namespace ok::fs;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "replay shape model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)
namespace
{
using Family = Ext4ReplayFamily;
using Verdict = Ext4ReplaySemanticVerdict;
struct Fixture
{
    Ext4RecoveryInfo binding{};
    std::array<u32, 2> directories{535, 536};
    std::array<jbd2::ReplayAction, 8> actions{};
    Fixture()
    {
        binding.layout.block_size = 1024; binding.layout.filesystem_blocks = 32768;
        binding.layout.journal_first_block = 16387; binding.layout.journal_blocks = 1024;
        binding.journal_inode = 8; binding.metadata_count = 16;
        for (usize group = 0; group < 4; ++group)
        {
            const u64 begin = 1 + group * 8192;
            binding.metadata[group * 4] = {begin, 2};
            binding.metadata[group * 4 + 1] = {begin + 2, 1};
            binding.metadata[group * 4 + 2] = {begin + 3, 1};
            binding.metadata[group * 4 + 3] = {begin + 4, 512};
        }
        // The journal cannot overlap group 2 metadata.
        binding.layout.journal_first_block = 17000;
    }
    jbd2::ReplayPlan plan(std::initializer_list<u32> targets)
    {
        usize index = 0;
        for (u32 target : targets)
        {
            CHECK(index < actions.size());
            actions[index] = {target, u32(index + 2), 31, 0, 0, false}; ++index;
        }
        return {std::span<const jbd2::ReplayAction>{actions}.first(index), 1, 32, 12, 0, false, false};
    }
    Result<Ext4ReplayShape> classify(const jbd2::ReplayPlan &plan)
    { return Ext4ReplayShapePlanner::classify(binding, {directories}, plan); }
    Result<Ext4ReplayShape> shape(std::initializer_list<u32> targets) { return classify(plan(targets)); }
};
void recognized_orders()
{
    Fixture f;
    const auto data = f.shape({3, 2, 1, 8});
    CHECK(data && data.value().includes(Family::bounded_data_allocation));
    CHECK(!data.value().includes(Family::namespace_change));
    const auto across_group = f.shape({8195, 2, 1, 8200});
    CHECK(across_group && across_group.value().candidates == data.value().candidates);
    const auto structural = f.shape({8});
    CHECK(structural && structural.value().includes(Family::structural_metadata));
    const auto create = f.shape({8, 4, 2, 1, 535});
    CHECK(create && create.value().includes(Family::empty_create));
    const auto create_distinct = f.shape({8, 9, 4, 2, 1, 535});
    CHECK(create_distinct && create_distinct.value().candidates == create.value().candidates);
    const auto orphan = f.shape({8, 9, 535, 1});
    CHECK(orphan && orphan.value().includes(Family::namespace_change));
    CHECK(!orphan.value().includes(Family::bounded_data_allocation));
    CHECK(!orphan.value().includes(Family::absent_rename));
    const auto three_tables = f.shape({8, 9, 10, 535});
    CHECK(three_tables && three_tables.value().includes(Family::namespace_change));
    CHECK(!three_tables.value().includes(Family::absent_rename));
    CHECK(f.shape({8, 9, 10, 535, 1}));
    const auto shared_table = f.shape({8, 535});
    CHECK(shared_table && shared_table.value().includes(Family::absent_rename));
    CHECK(shared_table.value().includes(Family::namespace_change));
    const auto two_tables = f.shape({8, 9, 535});
    CHECK(two_tables && two_tables.value().candidates == shared_table.value().candidates);
}
void semantic_collision()
{
    Fixture f;
    const auto shape = f.shape({8, 535}); CHECK(shape);
    const std::array decisions{
        Ext4ReplaySemanticDecision{Family::absent_rename, Verdict::valid, {}},
        Ext4ReplaySemanticDecision{Family::namespace_change, Verdict::not_this_family, {}}};
    const auto selected = Ext4ReplayShapePlanner::select(shape.value(), decisions);
    CHECK(selected && selected.value() == Family::absent_rename);
    auto next = decisions;
    next[0].verdict = Verdict::not_this_family; next[1].verdict = Verdict::valid;
    const auto namespace_selected = Ext4ReplayShapePlanner::select(shape.value(), next);
    CHECK(namespace_selected && namespace_selected.value() == Family::namespace_change);
    next = decisions; next[1].verdict = Verdict::invalid_transition;
    next[1].failure = Status::io_error("canonical namespace checksum mismatch");
    const auto rejected = Ext4ReplayShapePlanner::select(shape.value(), next);
    CHECK(!rejected && rejected.status().message() == next[1].failure.message());
    // An earlier accepted proof cannot hide a recognized rejected candidate.
    std::swap(next[0], next[1]); CHECK(!Ext4ReplayShapePlanner::select(shape.value(), next));
    next = decisions; next[1].verdict = Verdict::valid;
    CHECK(!Ext4ReplayShapePlanner::select(shape.value(), next));
    next = decisions; next[0].verdict = Verdict::not_this_family;
    CHECK(!Ext4ReplayShapePlanner::select(shape.value(), next));
    CHECK(!Ext4ReplayShapePlanner::select(shape.value(), std::span{decisions}.first(1)));
    next = decisions; next[1] = next[0]; CHECK(!Ext4ReplayShapePlanner::select(shape.value(), next));
    next = decisions; next[1].family = Family::bounded_data_allocation;
    CHECK(!Ext4ReplayShapePlanner::select(shape.value(), next));
    next = decisions; next[0].failure = Status::io_error("invalid valid status");
    CHECK(!Ext4ReplayShapePlanner::select(shape.value(), next));
    next = decisions; next[0].verdict = Verdict(99);
    CHECK(!Ext4ReplayShapePlanner::select(shape.value(), next));
    CHECK(!Ext4ReplayShapePlanner::select({0, false}, {}));
    CHECK(!Ext4ReplayShapePlanner::select({0x80000000U, false}, {}));
    CHECK(!Ext4ReplayShapePlanner::select({1, false}, {}));
}
void directory_create_marker_and_order()
{
    Fixture f;
    std::array<u8, 1024> descriptor{};
    const std::array<u8, 8> marker{'O', 'K', 'D', 'R', 'W', '0', '0', '1'};
    for (usize i = 0; i < marker.size(); ++i) descriptor[160 + i] = marker[i];
    descriptor[172] = 7;
    auto plan = f.plan({8, 4, 2, 1, 3, 600, 535});
    CHECK(!f.classify(plan)); // Seven homes never enter a legacy regular route.
    const auto classify = [&]() {
        return Ext4ReplayShapePlanner::classify(f.binding, {f.directories}, plan, &descriptor);
    };
    auto shape = classify();
    CHECK(shape && shape.value().includes(Family::bounded_directory_create));
    CHECK(!shape.value().includes(Family::empty_create));
    const std::array decision{
        Ext4ReplaySemanticDecision{Family::bounded_directory_create, Verdict::valid, {}}};
    CHECK(Ext4ReplayShapePlanner::select(shape.value(), decision));
    auto rejected = decision;
    rejected[0].verdict = Verdict::invalid_transition;
    rejected[0].failure = Status::io_error("directory complete witness rejected");
    CHECK(!Ext4ReplayShapePlanner::select(shape.value(), rejected));
    descriptor[172] = 8; CHECK(!classify());
    plan = f.plan({8, 9, 4, 2, 1, 3, 600, 535}); CHECK(classify());
    descriptor[160] = 0; CHECK(!classify()); descriptor[160] = 'O';
    plan = f.plan({8, 9, 4, 2, 1, 600, 3, 535}); CHECK(!classify());
    plan = f.plan({8, 9, 4, 2, 1, 3, 10, 535}); CHECK(!classify());
    plan = f.plan({8, 9, 4, 2, 1, 3, 0, 535}); CHECK(!classify());
    plan = f.plan({8, 9, 4, 2, 1, 3, 8193, 535}); CHECK(!classify());
    plan = f.plan({8, 9, 4, 2, 1, 3, 17000, 535}); CHECK(!classify());
    plan = f.plan({8, 9, 4, 2, 1, 3, 535, 535}); CHECK(!classify());
    plan = f.plan({8, 4, 2, 1, 535}); CHECK(!classify());
}
void malformed_plan()
{
    Fixture f;
    CHECK(!f.shape({8, 8, 535}));
    CHECK(!f.shape({1, 3, 2, 8}));
    CHECK(!f.shape({1, 2, 3, 8}));
    CHECK(!f.shape({1, 2, 4, 8, 535}));
    CHECK(!f.shape({1, 2, 4, 8, 9, 535}));
    CHECK(!f.shape({535, 8}));
    CHECK(!f.shape({8, 535, 9}));
    CHECK(!f.shape({8, 535, 536}));
    CHECK(!f.shape({8, 600}));
    CHECK(!f.shape({8193})); // Backup superblocks have no writer shape.
    CHECK(!f.shape({17000}));
    CHECK(!f.shape({32768}));
    CHECK(!f.shape({1}));
    CHECK(!f.shape({8, 9, 10, 11, 535}));
    CHECK(!f.shape({1, 2, 3, 8, 9, 10, 535}));
    auto plan = f.plan({3, 2, 1, 8});
    plan.incomplete_tail = true; CHECK(!f.classify(plan));
    plan.incomplete_tail = false; plan.revoked_actions = 1; CHECK(!f.classify(plan));
    plan.revoked_actions = 0; plan.revoke_records = 1; CHECK(!f.classify(plan));
    plan.revoke_records = 0; plan.committed_transactions = 2; CHECK(!f.classify(plan));
    plan.committed_transactions = 0; CHECK(!f.classify(plan));
    plan.committed_transactions = 1; f.actions[1].sequence = 32; CHECK(!f.classify(plan));
    f.actions[1].sequence = 31; f.actions[1].transaction_order = 1; CHECK(!f.classify(plan));
    f.actions[1].transaction_order = 0; f.actions[1].journal_block = f.actions[0].journal_block;
    CHECK(!f.classify(plan));
    f.actions[1].journal_block = 0; CHECK(!f.classify(plan));
    f.actions[1].journal_block = 1024; CHECK(!f.classify(plan));
    plan = f.plan({}); CHECK(!f.classify(plan));
    plan.empty_log = true; plan.committed_transactions = 0;
    const auto empty = f.classify(plan); CHECK(empty && empty.value().empty_log);
    const auto selected = Ext4ReplayShapePlanner::select(empty.value(), {});
    CHECK(selected && selected.value() == Family::empty);
    plan.revoked_actions = 1; CHECK(!f.classify(plan));
    plan.revoked_actions = 0; plan.revoke_records = 1; CHECK(!f.classify(plan));
    plan.revoke_records = 0; plan.incomplete_tail = true; CHECK(!f.classify(plan));
    plan = f.plan({8}); plan.empty_log = true; CHECK(!f.classify(plan));
}
void malformed_domains()
{
    Fixture f;
    f.directories[1] = f.directories[0]; CHECK(!f.shape({8, 535}));
    f.directories[1] = 8; CHECK(!f.shape({8, 535}));
    f.directories[1] = 17000; CHECK(!f.shape({8, 535}));
    f.directories[1] = 32768; CHECK(!f.shape({8, 535}));
    f.directories[1] = 536; f.binding.metadata[2].first = 3; CHECK(!f.shape({8, 535}));
    f.binding.metadata[2].first = 4; f.binding.metadata[3].count = 511; CHECK(!f.shape({8, 535}));
    f.binding.metadata[3].count = 512; f.binding.metadata[5].first = 3; CHECK(!f.shape({8, 535}));
    f.binding.metadata[5].first = 8195; f.binding.layout.journal_first_block = 5;
    CHECK(!f.shape({8, 535}));
    f.binding.layout.journal_first_block = 17000; f.binding.metadata_count = 15;
    CHECK(!f.shape({8, 535}));
    f.binding.metadata_count = 16; f.binding.layout.block_size = 4096; CHECK(!f.shape({8, 535}));
}
}
int main()
{
    recognized_orders(); semantic_collision(); directory_create_marker_and_order(); malformed_plan(); malformed_domains();
    std::puts("PASS bounded replay shape model: orders, collisions, terminal rejection and domains");
}
