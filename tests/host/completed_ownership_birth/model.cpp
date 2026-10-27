// Safe hosted lifecycle/field-comparison model. Device callbacks only observe
// seal completion; altered copied fields are tested by pure comparisons only.
#include "ext4_zero_truncate_preview_test_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace zero_preview_fixture;
namespace
{
using Mount = Ext4MountedMetadata<2>;
using Owner = Mount::Owner;
using Workspace = Owner::TypedOwnerWorkspace;
using Validator = Ext4ZeroTruncateOwnershipValidator;
using Birth = Validator::CompletedBirthSeal;
using BirthProgress = Validator::CompletedBirthProgress;
using Snapshot = Mount::TypedOwnershipSnapshot;
usize checks{}, fixtures{}, comparisons{}, global_callbacks{}, target_callbacks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "ownership birth:%d: %s\n", __LINE__, #x); std::abort(); } } while (false)
static_assert(!std::is_copy_constructible_v<Birth> && !std::is_move_constructible_v<Birth>);
static_assert(!std::is_copy_constructible_v<BirthProgress> && !std::is_move_constructible_v<BirthProgress>);
template <class T> class Bytes
{
    std::unique_ptr<unsigned char[]> bytes_{new unsigned char[sizeof(T)]};
  public:
    explicit Bytes(const T &value) { std::memcpy(bytes_.get(), &value, sizeof(T)); }
    bool same(const T &value) const { return !std::memcmp(bytes_.get(), &value, sizeof(T)); }
};
void be32(Block &bytes, usize at, u32 value) {
    for (usize byte = 0; byte < 4; ++byte) bytes[at + byte] = u8(value >> (24 - byte * 8));
}
Block data{};
Ext4ZeroTruncatePlan zero_plan{};
Ext4ZeroTruncatePlan zero_scratch{};
Ext4ZeroTruncateJournalFormat zero_journal{};
Ext4ZeroTruncateJournalFormatScratch zero_format{};
Ext4AllocatedEmptyDataScratch allocated_scratch{};
Ext4AllocatedEmptyDataPlan allocated_plan{};
Ext4AllocatedEmptyDataJournalFormat allocated_journal{};
Ext4AllocatedEmptyDataJournalFormatScratch allocated_format{};
void media(Image &image, bool allocated, bool recovery, u8 mask, u32 count) {
    auto &head = image.blocks[1000];
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1); be32(head, 24, 70);
    be32(head, 40, jbd2::feature_checksum_v3 | (allocated ? jbd2::feature_revoke : 0));
    be32(head, 64, 1); head[80] = 4;
    for (usize at = 0; at < 16; ++at) head[48 + at] = image.f.binding.uuid[at];
    be32(head, 252, jbd2::detail::crc32c(~u32{0}, head));
    image.f.binding.empty_log = true; image.f.binding.journal_sequence = 70;
    data = {};
    for (usize at = 7; at < 73; ++at) data[at] = u8(1 + at % 251);
    if (allocated) {
        const Ext4AllocatedEmptyDataSource source{image.f.group, image.f.cross ? image.f.physical : 70, 8,
            image.blocks[1], image.blocks[2], image.blocks[image.f.cross ? 8195 : 3], image.blocks[4], image.blocks[8]};
        CHECK(Ext4AllocatedEmptyDataPlanner::plan_off_stack(image.f.binding, {13, 9, 73, 1700000222, 123456789},
            source, allocated_scratch, allocated_plan));
        CHECK(Ext4AllocatedEmptyDataJournalFormatter::format(image.f.binding, allocated_plan, head,
            image.blocks[4], allocated_format, allocated_journal));
    } else if (count) {
        const Ext4ZeroTruncateSource source{image.f.group, 8, image.blocks[1], image.blocks[2],
            image.blocks[image.f.cross ? 8195 : 3], image.blocks[4], image.blocks[8]};
        CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(image.f.binding, image.update(), source, zero_scratch, zero_plan));
        CHECK(Ext4ZeroTruncateJournalFormatter::format(image.f.binding, zero_plan, head, image.blocks[4], zero_format, zero_journal));
    }
    if (recovery) {
        image.blocks[1000] = allocated ? allocated_journal.head : zero_journal.head;
        image.blocks[1001] = allocated ? allocated_journal.descriptor : zero_journal.descriptor;
        for (u32 at = 0; at < 4; ++at) image.blocks[1002 + at] = allocated ? allocated_journal.payloads[at] : zero_journal.payloads[at];
        image.blocks[1006] = allocated ? allocated_journal.commit : zero_journal.commit;
        image.blocks[1007] = allocated ? allocated_journal.end : zero_journal.end;
        for (u32 home = 0; home < 4; ++home) {
            const u32 target = allocated ? allocated_plan.targets[home] : zero_plan.targets[home];
            image.blocks[target] = allocated ? (mask & (1U << home) ? allocated_plan.after[home] : allocated_plan.before[home]) :
                (mask & (1U << home) ? zero_plan.after[home] : zero_plan.before[home]);
        }
        if (allocated) image.blocks[allocated_plan.physical] = data;
        image.f.binding.empty_log = false;
        image.f.binding.journal_sequence = allocated ? allocated_journal.active_sequence : zero_journal.active_sequence;
    }
}

struct Device final : driver::BlockDevice
{
    std::array<Block, 8705> &blocks;
    Workspace *workspace{};
    bool allocated{};
    usize reads{}, writes{}, flushes{}, global_reads{}, target_reads{};
    std::unique_ptr<Bytes<Birth>> completed_bytes;
    explicit Device(std::array<Block, 8705> &value) : blocks(value) {}
    template <typename Child> void observe(const Child &child)
    {
        const auto &birth = child.ownership_birth_;
        CHECK(Validator::completed_birth_progress_exact(birth, child.ownership_birth_progress_, &child));
        CHECK(child.completed_ownership_birth_valid());
        if (!birth.global_minted_) return;
        CHECK(child.completed_ownership_birth_valid());
        if (!birth.target_minted_) { ++global_reads; ++global_callbacks; }
        else
        {
            ++target_reads; ++target_callbacks;
            if (!completed_bytes) completed_bytes.reset(new Bytes<Birth>(birth));
            CHECK(completed_bytes->same(birth));
        }
    }
    driver::BlockGeometry geometry() const override { return {blocks.size() * 2, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> output) override
    {
        ++reads;
        if (workspace) { if (allocated) observe(workspace->allocated_); else observe(workspace->zero_); }
        if (sector % 2 || output.size() != 1024 || sector / 2 >= blocks.size()) return Status::invalid_argument("birth model read");
        for (usize at = 0; at < 1024; ++at) output[at] = std::byte(blocks[sector / 2][at]);
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> input) override
    {
        ++writes;
        if (sector % 2 || input.size() != 1024 || sector / 2 >= blocks.size()) return Status::invalid_argument("birth model write");
        for (usize at = 0; at < 1024; ++at) blocks[sector / 2][at] = std::to_integer<u8>(input[at]);
        return {};
    }
    Status flush() override { ++flushes; return {}; }
};
template <typename Child>
void field_comparisons(Child &child, const auto &view, const auto &proof, const auto &binding)
{
    auto &birth = child.ownership_birth_;
    const Bytes unchanged(birth);
    const auto exact = [&] { return Validator::completed_birth_exact(view, child.reader_, binding, proof, birth, &child, true); };
    CHECK(exact());
    const auto field = [&](auto &value)
    {
        const auto original = value; value ^= 1;
        CHECK(!exact()); value = original; CHECK(exact()); ++comparisons;
    };
    const auto pointer = [&](auto &value, auto foreign)
    {
        const auto original = value; value = foreign;
        CHECK(!exact()); value = original; CHECK(exact()); ++comparisons;
    };
    // Only saved addresses change. The pure comparison never follows them.
    pointer(birth.child_, static_cast<const void *>(&binding));
    pointer(birth.view_, static_cast<const void *>(&proof));
    pointer(birth.reader_, static_cast<const void *>(&view));
    pointer(birth.proof_, static_cast<const void *>(&child.reader_));
    Ext4RecoveryInfo other_binding{}; pointer(birth.original_binding_, &other_binding);
    field(birth.seed_); field(birth.first_data_); field(birth.blocks_per_group_); field(birth.inodes_per_group_);
    field(birth.group_count_); field(birth.inodes_); field(birth.first_inode_); field(birth.table_blocks_);
    field(birth.retained_orphan_); field(birth.retained_orphan_generation_); field(birth.retained_orphan_next_);
    field(birth.attempted_); field(birth.ready_); field(birth.failed_); field(birth.global_minted_); field(birth.target_minted_);
    field(birth.range_count_);
    const auto layout = [&](auto &value)
    {
        field(value.first_sector); field(value.filesystem_blocks); field(value.journal_first_block);
        field(value.journal_blocks); field(value.block_size);
    };
    layout(birth.layout_); layout(birth.binding_.layout);
    for (auto &byte : birth.uuid_) field(byte);
    for (auto &byte : birth.binding_.uuid) field(byte);
    field(birth.binding_.metadata_count); field(birth.binding_.journal_inode); field(birth.binding_.journal_sequence);
    field(birth.binding_.needs_recovery); field(birth.binding_.empty_log);
    for (auto &group : birth.groups_)
    {
        field(group.block_bitmap); field(group.inode_bitmap); field(group.table);
        field(group.block_checksum); field(group.inode_checksum); field(group.flags);
    }
    for (auto &range : birth.ranges_) { field(range.first); field(range.count); }
    for (auto &range : birth.binding_.metadata) { field(range.first); field(range.count); }
    field(birth.target_.inode); field(birth.target_.generation); field(birth.target_.old_size);
    field(birth.target_.count); field(birth.target_.group);
    for (auto &mapping : birth.target_.mapping) field(mapping);
    CHECK(unchanged.same(birth));
}
void run(bool allocated, bool recovery, bool cross, u32 count, u8 mask, bool compare_fields)
{
    Image image(allocated ? 0 : count, cross); media(image, allocated, recovery, mask, count);
    Device device(image.blocks); Owner::Registry registry;
    auto workspace = std::unique_ptr<Workspace>(new Workspace);
    auto snapshot = std::unique_ptr<Snapshot>(new Snapshot);
    auto mount = std::unique_ptr<Mount>(new Mount(Ext4MountedPolicy::ordered_data_dirty_noatime));
    const auto registration = registry.register_device(device, 100 + fixtures); CHECK(registration);
    CHECK(mount->owner_.open(registry, registration.value(), {0, device.geometry().block_count}));
    for (u64 block : {u64{1}, u64{8}, u64{71}, u64(!allocated && count ?
            (cross ? image.f.physical + 2 : 72) : 900)})
    {
        Owner::Pin pin; CHECK(mount->owner_.pin(block, pin)); CHECK(pin.release());
    }
    const auto before = device.reads;
    CHECK(!mount->capture_typed_ownership_snapshot(*workspace, *snapshot)); CHECK(device.reads == before);
    device.workspace = workspace.get(); device.allocated = allocated;
    CHECK(allocated ? mount->owner_.run_typed_allocated(*workspace, image.f.binding,
          {13, 9, 73, 1700000222, 123456789}, 7, data, recovery) :
          mount->owner_.run_typed_zero(*workspace, image.f.binding, image.update(), recovery));
    CHECK(device.global_reads && device.target_reads && device.completed_bytes);
    const auto after = device.reads + device.writes + device.flushes;
    CHECK(workspace->published_cache_.minted_);
    CHECK(mount->capture_typed_ownership_snapshot(*workspace, *snapshot));
    CHECK(mount->validate_typed_ownership_snapshot(*workspace, *snapshot));
    CHECK(snapshot->allocated == allocated && snapshot->noop == (!allocated && !count && !recovery));
    CHECK(snapshot->range_count == (allocated ? 2U : 1U) && snapshot->target.count == (allocated ? 1U : 0U));
    const auto completed = [&](auto &child, const auto &view, const auto &proof, const auto &binding)
    {
        CHECK(child.completed_ownership_birth_valid(true)); CHECK(device.completed_bytes->same(child.ownership_birth_));
        CHECK(Validator::completed_birth_complete(child.ownership_birth_));
        CHECK(Validator::completed_birth_progress_exact(child.ownership_birth_, child.ownership_birth_progress_, &child));
        CHECK(child.ownership_birth_progress_.stage_ == BirthProgress::Stage::target);
        CHECK(child.completed_owner_callbacks_retired());
        if (compare_fields) field_comparisons(child, view, proof, binding);
        CHECK(device.completed_bytes->same(child.ownership_birth_));
    };
    if (allocated) completed(workspace->allocated_, workspace->allocated_.final_view_, workspace->allocated_.final_proof_, workspace->allocated_.final_binding_);
    else if (!count && !recovery) completed(workspace->zero_, workspace->zero_.old_view_, workspace->zero_.old_proof_, workspace->zero_.binding_);
    else completed(workspace->zero_, workspace->zero_.final_view_, workspace->zero_.final_proof_, workspace->zero_.final_binding_);
    CHECK(mount->validate_typed_ownership_snapshot(*workspace, *snapshot));
    CHECK(after == device.reads + device.writes + device.flushes);
    CHECK(mount->owner_.close()); CHECK(registry.detach(registration.value())); ++fixtures;
}
} // namespace
int main()
{
    for (bool cross : {false, true})
    {
        run(false, false, cross, 0, 0, true);
        run(false, false, cross, 3, 0, true);
        run(true, false, cross, 0, 0, true);
        for (u8 mask : {u8{0}, u8{5}, u8{15}})
        {
            run(false, true, cross, 3, mask, false);
            run(true, true, cross, 0, mask, false);
        }
    }
    std::printf("COMPLETED_OWNERSHIP_BIRTH fixtures=%zu checks=%zu comparisons=%zu global_callbacks=%zu target_callbacks=%zu\n",
                fixtures, checks, comparisons, global_callbacks, target_callbacks);
}
