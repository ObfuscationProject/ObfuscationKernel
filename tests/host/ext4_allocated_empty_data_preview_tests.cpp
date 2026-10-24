#include "ext4_allocated_empty_preview_test_support.hpp"
#include <ok/fs/ext4_allocated_empty_data_journal.hpp>
#include <ok/fs/ext4_allocated_empty_data_preview.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>

using namespace allocated_preview_fixture;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "allocated empty preview:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
namespace
{
struct Reader
{
    Image &image;
    std::span<const Block> blocks{image.blocks};
    usize attempts{}, fail_at{}, drift_at{};
    bool partial{}, alternate_error{};
    u32 forbidden{~u32{0}};
    Status read_block(u32 number, std::span<u8> out)
    {
        ++attempts;
        CHECK(number != forbidden);
        if (number >= blocks.size() || out.size() != 1024)
            return Status::io_error("preview model bounds");
        if (attempts == fail_at)
        {
            if (partial) for (usize byte = 0; byte < 511; ++byte) out[byte] = blocks[number][byte];
            return alternate_error ? Status::not_initialized("preview model read unavailable") :
                Status::io_error("preview model read failed");
        }
        for (usize byte = 0; byte < 1024; ++byte) out[byte] = blocks[number][byte];
        if (attempts == drift_at) out[900] ^= 1;
        return {};
    }
};
using Preview = Ext4AllocatedEmptyDataPreview<Reader>;
using Workspace = Ext4AllocatedEmptyDataPreviewWorkspace<Reader>;
static_assert(!std::is_copy_constructible_v<Workspace> && !std::is_move_constructible_v<Workspace>);
static_assert(!std::is_copy_constructible_v<Preview> && !std::is_move_constructible_v<Preview>);
Workspace workspace;
Ext4AllocatedEmptyDataScratch planner;
Ext4AllocatedEmptyDataPlan plan;
Ext4AllocatedEmptyDataJournalFormatScratch formatter;
Ext4AllocatedEmptyDataJournalFormat journal;
std::array<Block, 8193> preserved;
usize positives{}, mixtures{}, failures{}, drifts{}, claims{}, refusals{}, baseline{};
std::array<Block, 8705> cross_disk{}, cross_before{}, cross_preserved{};
usize cross_reads{};
void be32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
void reset()
{ std::destroy_at(&workspace); std::construct_at(&workspace); }
void prepare(Image &image, u8 mask, u32 size = 73)
{
    const Ext4AllocatedEmptyDataUpdate update{13, 9, size, 1700000222, 123456789};
    const Ext4AllocatedEmptyDataSource source{0, 70, 8, image.blocks[1], image.blocks[2],
        image.blocks[3], image.blocks[4], image.blocks[8]};
    CHECK(Ext4AllocatedEmptyDataPlanner::plan_off_stack(image.f.binding, update, source, planner, plan));
    Block head{};
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1); be32(head, 24, 70);
    be32(head, 40, jbd2::feature_checksum_v3 | jbd2::feature_revoke);
    for (usize at = 0; at < image.f.binding.uuid.size(); ++at) head[48 + at] = image.f.binding.uuid[at];
    be32(head, 64, 1); head[80] = 4;
    be32(head, 252, jbd2::detail::crc32c(~u32{0}, head));
    image.f.binding.journal_sequence = 70;
    CHECK(Ext4AllocatedEmptyDataJournalFormatter::format(image.f.binding, plan, head,
        image.blocks[4], formatter, journal));
    image.f.binding.empty_log = false; image.f.binding.journal_sequence = journal.active_sequence;
    image.blocks[1000] = journal.head; image.blocks[1001] = journal.descriptor;
    for (usize index = 0; index < 4; ++index) image.blocks[1002 + index] = journal.payloads[index];
    image.blocks[1006] = journal.commit; image.blocks[1007] = journal.end;
    for (usize home = 0; home < 4; ++home)
        image.blocks[plan.targets[home]] = mask & (1U << home) ? plan.after[home] : plan.before[home];
    for (usize byte = 0; byte < size; ++byte) image.blocks[plan.physical][byte] = u8((byte * 17 + 3) % 251);
    preserved = image.blocks;
}
void positives_and_claims()
{
    for (u32 size : {1U, 73U, 1024U}) for (u8 mask = 0; mask < 16; ++mask)
    {
        Image image(0); prepare(image, mask, size); Reader reader{image}; reset(); Preview preview;
        CHECK(preview.prepare(reader, image.f.binding, workspace));
        const auto &report = preview.report();
        CHECK(report.phase == Ext4AllocatedEmptyDataPreviewPhase::ready && report.old_ownership &&
            report.final_ownership && report.snapshot_readback && report.inode == 13 && report.generation == 9 &&
            report.physical == 70 && report.group == 0 && report.size == size && report.old_version == 42 &&
            report.new_version == 43 && report.after_mask == mask && report.active_sequence == 71 &&
            report.next_sequence == 72 && report.read_attempts == reader.attempts &&
            report.completed_reads == reader.attempts && image.blocks == preserved);
        CHECK(!baseline || reader.attempts == baseline); baseline = reader.attempts;
        CHECK(baseline <= 2876 + 1062);
        const auto attempts = reader.attempts;
        CHECK(!preview.prepare(reader, image.f.binding, workspace));
        CHECK(reader.attempts == attempts && preview.report().phase == Ext4AllocatedEmptyDataPreviewPhase::ready);
        Preview second;
        CHECK(!second.prepare(reader, image.f.binding, workspace));
        CHECK(second.report().phase == Ext4AllocatedEmptyDataPreviewPhase::failed && reader.attempts == attempts);
        ++positives; ++mixtures; claims += 2;
    }
}
void faults_and_bookends()
{
    for (usize at = 1; at <= baseline; ++at) for (bool partial : {false, true}) for (bool alternate : {false, true})
    {
        Image image(0); prepare(image, 0); Reader reader{image}; reader.fail_at = at;
        reader.partial = partial; reader.alternate_error = alternate;
        reset(); Preview preview;
        CHECK(!preview.prepare(reader, image.f.binding, workspace));
        const auto &report = preview.report();
        CHECK(report.phase == Ext4AllocatedEmptyDataPreviewPhase::failed && report.read_attempts == at &&
            report.completed_reads == at - 1 && reader.attempts == at && image.blocks == preserved);
        Preview other; CHECK(!other.prepare(reader, image.f.binding, workspace)); CHECK(reader.attempts == at);
        ++failures; ++claims;
    }
    for (usize index = 0; index < 14; ++index)
    {
        Image image(0); prepare(image, 5); Reader reader{image}; reader.drift_at = baseline - 13 + index;
        reset(); Preview preview; CHECK(!preview.prepare(reader, image.f.binding, workspace));
        CHECK(preview.report().phase == Ext4AllocatedEmptyDataPreviewPhase::failed && !preview.report().snapshot_readback &&
            reader.attempts == reader.drift_at && preview.report().completed_reads == reader.attempts && image.blocks == preserved);
        ++drifts;
    }
}
void refusals_and_aliases()
{
    for (usize home = 0; home < 4; ++home)
    {
        Image image(0); prepare(image, 0); image.blocks[plan.targets[home]][777] ^= 1;
        Reader reader{image}; reset(); Preview preview; CHECK(!preview.prepare(reader, image.f.binding, workspace));
        CHECK(preview.report().phase == Ext4AllocatedEmptyDataPreviewPhase::failed && reader.attempts == 13);
        ++refusals;
    }
    {
        Image image(0); prepare(image, 0); image.blocks[1007][7] = 1;
        Reader reader{image}; reader.forbidden = plan.targets[0]; reset(); Preview preview;
        CHECK(!preview.prepare(reader, image.f.binding, workspace)); CHECK(reader.attempts == 8); ++refusals;
    }
    for (usize field = 0; field < 5; ++field)
    {
        Image image(0); prepare(image, 0); auto binding = image.f.binding;
        if (field == 0) binding.needs_recovery = false;
        if (field == 1) binding.empty_log = true;
        if (field == 2) binding.layout.block_size = 2048;
        if (field == 3) binding.layout.journal_first_block = binding.layout.filesystem_blocks;
        if (field == 4) binding.layout.journal_blocks = 8;
        Reader reader{image}; reset(); Preview preview;
        CHECK(!preview.prepare(reader, binding, workspace)); CHECK(!reader.attempts); ++refusals;
    }
    {
        Image image(0); prepare(image, 0); Reader reader{image}; reset(); Preview preview;
        auto &binding = *reinterpret_cast<Ext4RecoveryInfo *>(&workspace);
        CHECK(!preview.prepare(reader, binding, workspace)); CHECK(!reader.attempts);
        CHECK(preview.report().phase == Ext4AllocatedEmptyDataPreviewPhase::unused); ++refusals;
        auto &same_reader = *reinterpret_cast<Reader *>(&workspace);
        CHECK(!preview.prepare(same_reader, image.f.binding, workspace)); CHECK(!reader.attempts);
        CHECK(preview.report().phase == Ext4AllocatedEmptyDataPreviewPhase::unused); ++refusals;
    }
}
void prepare_cross(Image &image, Fixture &cross, bool peer)
{
    cross_disk.fill(Block{});
    for (usize at = 0; at < image.blocks.size(); ++at) cross_disk[at] = image.blocks[at];
    cross_disk[3].fill(0xff); cross_disk[8195] = cross.bitmap; cross_disk[8196].fill(0);
    cross_disk[1] = cross.superblock; cross_disk[2] = cross.descriptors;
    put16(cross_disk[2], 14, 253); put16(cross_disk[2], 16, 1);
    put16(cross_disk[2], 26, u16(jbd2::detail::crc32c(cross.seed, std::span<const u8>{cross_disk[4]}.first(32))));
    put16(cross_disk[2], 24, u16(jbd2::detail::crc32c(cross.seed, cross_disk[3])));
    put16(cross_disk[2], 58, u16(jbd2::detail::crc32c(cross.seed, std::span<const u8>{cross_disk[8196]}.first(32))));
    if (peer)
    {
        auto node = std::span<u8>{cross_disk[8197]}.first(256);
        for (usize byte = 0; byte < 256; ++byte) node[byte] = cross_disk[8][byte];
        put32(node, 4, 1); put32(node, 28, 2); put16(node, 42, 1); put16(node, 56, 1); put32(node, 60, 8261);
        stamp_inode(node, 257, cross.seed); cross_disk[8196][0] = 1;
        put16(cross_disk[2], 46, 255);
        put16(cross_disk[2], 58, u16(jbd2::detail::crc32c(cross.seed, std::span<const u8>{cross_disk[8196]}.first(32))));
    }
    stamp_descriptor(std::span<u8>{cross_disk[2]}.first(32), 0, cross.seed);
    stamp_descriptor(std::span<u8>{cross_disk[2]}.subspan(32, 32), 1, cross.seed);
    put32(cross_disk[1], 16, peer ? 508 : 509);
    put32(cross_disk[1], 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{cross_disk[1]}.first(1020)));
    const Ext4AllocatedEmptyDataUpdate update{13, 9, 73, 1700000222, 123456789};
    const Ext4AllocatedEmptyDataSource source{1, 8261, 8, cross_disk[1], cross_disk[2],
        cross_disk[8195], cross_disk[4], cross_disk[8]};
    CHECK(Ext4AllocatedEmptyDataPlanner::plan_off_stack(cross.binding, update, source, planner, plan));
    Block head{};
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1); be32(head, 24, 70);
    be32(head, 40, jbd2::feature_checksum_v3);
    for (usize at = 0; at < cross.binding.uuid.size(); ++at) head[48 + at] = cross.binding.uuid[at];
    be32(head, 64, 1); head[80] = 4; be32(head, 252, jbd2::detail::crc32c(~u32{0}, head));
    cross.binding.journal_sequence = 70;
    CHECK(Ext4AllocatedEmptyDataJournalFormatter::format(cross.binding, plan, head, cross_disk[4], formatter, journal));
    cross.binding.empty_log = false; cross.binding.journal_sequence = journal.active_sequence;
    cross_disk[1000] = journal.head; cross_disk[1001] = journal.descriptor;
    for (usize index = 0; index < 4; ++index) cross_disk[1002 + index] = journal.payloads[index];
    cross_disk[1006] = journal.commit; cross_disk[1007] = journal.end;
    cross_disk[8261].fill(0x5a); cross_before = cross_disk;
}
void cross_group_controls()
{
    Image image(0); Fixture cross(true); prepare_cross(image, cross, false);
    for (u8 mask = 0; mask < 16; ++mask)
    {
        cross_disk = cross_before;
        for (usize home = 0; home < 4; ++home)
            cross_disk[plan.targets[home]] = mask & (1U << home) ? plan.after[home] : plan.before[home];
        cross_preserved = cross_disk;
        Reader reader{image}; reader.blocks = cross_disk; reset(); Preview preview;
        CHECK(preview.prepare(reader, cross.binding, workspace));
        CHECK(preview.report().after_mask == mask && preview.report().physical == 8261 &&
            preview.report().group == 1 && preview.report().old_ownership && preview.report().final_ownership &&
            cross_disk == cross_preserved && reader.attempts <= 2876 + 1062 * 2);
        CHECK(!cross_reads || reader.attempts == cross_reads); cross_reads = reader.attempts;
        ++positives; ++mixtures;
    }
    for (usize at = 1; at <= cross_reads; ++at)
    {
        cross_disk = cross_before; cross_preserved = cross_disk;
        Reader reader{image}; reader.blocks = cross_disk; reader.fail_at = at; reader.partial = true;
        reset(); Preview preview; CHECK(!preview.prepare(reader, cross.binding, workspace));
        CHECK(reader.attempts == at && preview.report().completed_reads == at - 1 && cross_disk == cross_preserved); ++failures;
    }
    Image other(0); Fixture peer_cross(true); prepare_cross(other, peer_cross, true);
    Reader reader{other}; reader.blocks = cross_disk; reset(); Preview preview;
    CHECK(!preview.prepare(reader, peer_cross.binding, workspace));
    CHECK(preview.report().phase == Ext4AllocatedEmptyDataPreviewPhase::failed && !preview.report().old_ownership); ++refusals;
}
}
int main()
{
    positives_and_claims(); faults_and_bookends(); refusals_and_aliases(); cross_group_controls();
    std::printf("ALLOCATED_EMPTY_PREVIEW_MODEL positive=%zu mixtures=%zu failures=%zu drifts=%zu claims=%zu refusals=%zu baseline=%zu cross_reads=%zu\n",
        positives, mixtures, failures, drifts, claims, refusals, baseline, cross_reads);
    return 0;
}
