#include "ext4_zero_truncate_preview_test_support.hpp"
#include <ok/fs/ext4_zero_truncate_preview.hpp>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace zero_preview_fixture;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "zero truncate preview model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
Ext4ZeroTruncatePlan planner{}, plan{};
Ext4ZeroTruncateWitnessScratch witness_scratch{};
Ext4ZeroTruncateJournalScratch journal_scratch{};
Ext4ZeroTruncateJournalResult journal_result{};
std::array<Block, 8705> saved{};
usize positive{}, refusal{}, read_faults{};
constexpr u32 sequence = 71, first = 1000;

void be32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
void stamp(Block &bytes, usize at, u32 seed)
{ be32(bytes, at, 0); be32(bytes, at, jbd2::detail::crc32c(seed, bytes)); }
void journal(Image &image, u32 mask)
{
    const auto request = image.update();
    const Ext4ZeroTruncateSource source{image.f.group, 8, image.blocks[1], image.blocks[2],
        image.blocks[image.f.group ? 8195 : 3], image.blocks[4], image.blocks[8]};
    CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(image.f.binding, request, source, planner, plan));
    auto &head = image.blocks[first];
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1);
    be32(head, 24, sequence); be32(head, 28, 1); be32(head, 40, jbd2::feature_checksum_v3);
    for (usize index = 0; index < image.f.binding.uuid.size(); ++index) head[48 + index] = image.f.binding.uuid[index];
    be32(head, 64, 1); head[80] = 4; stamp(head, 252, ~u32{0});
    auto &descriptor = image.blocks[first + 1];
    be32(descriptor, 0, jbd2::magic); be32(descriptor, 4, u32(jbd2::RecordType::descriptor)); be32(descriptor, 8, sequence);
    for (usize home = 0; home < 4; ++home)
    {
        auto &payload = image.blocks[first + 2 + home]; payload = plan.after[home];
        const bool escaped = jbd2::detail::be32(payload, 0) == jbd2::magic;
        if (escaped) be32(payload, 0, 0);
        const usize at = home ? 44 + (home - 1) * 16 : 12;
        const u32 flags = (home ? jbd2::flag_same_uuid : 0) | (home == 3 ? jbd2::flag_last_tag : 0) |
            (escaped ? jbd2::flag_escape : 0);
        be32(descriptor, at, plan.targets[home]); be32(descriptor, at + 4, flags);
        be32(descriptor, at + 12, jbd2::detail::crc32c(jbd2::detail::crc32c(image.f.seed,
            std::span<const u8>{descriptor}.subspan(8, 4)), payload));
    }
    CHECK(Ext4ZeroTruncateWitnessCodec::encode(image.f.binding, plan, image.blocks[4],
        sequence, witness_scratch, descriptor));
    stamp(descriptor, 1020, image.f.seed);
    auto &commit = image.blocks[first + 6];
    be32(commit, 0, jbd2::magic); be32(commit, 4, u32(jbd2::RecordType::commit)); be32(commit, 8, sequence);
    be32(commit, 52, request.seconds); be32(commit, 56, request.nanoseconds); stamp(commit, 16, image.f.seed);
    image.f.binding.empty_log = false; image.f.binding.journal_sequence = sequence;
    for (usize home = 0; home < 4; ++home)
        image.blocks[plan.targets[home]] = mask & (1U << home) ? plan.after[home] : plan.before[home];
}
Ext4ZeroTruncateJournalRecords records(const Image &image, usize count = 4)
{
    return {image.blocks[first], image.blocks[first + 1], image.blocks[first + 6], image.blocks[first + 7],
        std::span<const Block>{image.blocks}.subspan(first + 2, count)};
}
std::array<Block, 4> homes(const Image &image)
{
    std::array<Block, 4> out{};
    for (usize home = 0; home < 4; ++home) out[home] = image.blocks[plan.targets[home]];
    return out;
}
struct Reader
{
    Image &image;
    usize reads{}, fail_at{}, target_reads{}, change_target_at{}, head_reads{}, change_head_at{};
    Status read_block(u32 number, std::span<u8> out)
    {
        if (++reads == fail_at) return Status::io_error("injected zero truncate preview read failure");
        if (out.size() != 1024 || number >= image.blocks.size())
            return Status::invalid_argument("zero truncate preview model transport bounds");
        if (number == 8 && ++target_reads == change_target_at)
        {
            put32(image.inode(13), 100, 10); stamp_inode(image.inode(13), 13, image.f.seed);
        }
        if (number == first && ++head_reads == change_head_at)
        { be32(image.blocks[first], 24, sequence + 1); stamp(image.blocks[first], 252, ~u32{0}); }
        for (usize byte = 0; byte < 1024; ++byte) out[byte] = image.blocks[number][byte];
        return {};
    }
};
using Workspace = Ext4ZeroTruncatePreviewWorkspace<Reader>;
using Preview = Ext4ZeroTruncatePreview<Reader>;
alignas(Workspace) std::array<std::byte, sizeof(Workspace)> arena{};

usize preview(Image &image, StatusCode expected = StatusCode::ok, usize fault = 0, bool target_change = false, bool head_change = false)
{
    auto *workspace = ::new (arena.data()) Workspace{};
    Reader reader{image}; reader.fail_at = fault;
    reader.change_target_at = target_change ? 2 : 0; reader.change_head_at = head_change ? 2 : 0;
    Preview value;
    saved = image.blocks;
    const auto status = value.prepare(reader, image.f.binding, *workspace);
    if (status.code() != expected)
        std::fprintf(stderr, "preview expected=%u actual=%u phase=%u reads=%zu fault=%zu target_change=%u head_change=%u message=%.*s\n",
            u32(expected), u32(status.code()), u32(value.report().phase), reader.reads, fault,
            u32(target_change), u32(head_change), int(status.message().size()), status.message().data());
    CHECK(status.code() == expected);
    const auto report = value.report();
    CHECK(report.read_attempts == reader.reads);
    CHECK(report.completed_reads == reader.reads - (fault && fault <= reader.reads ? 1 : 0));
    if (status)
    {
        CHECK(report.phase == Ext4ZeroTruncatePreviewPhase::ready && report.old_ownership &&
            report.final_ownership && report.snapshot_readback && report.inode == 13 && report.generation == 9 &&
            report.released_count == plan.released_count && report.active_sequence == sequence && report.next_sequence == sequence + 1);
        CHECK(!value.prepare(reader, image.f.binding, *workspace));
        Preview other;
        const auto before = reader.reads;
        CHECK(!other.prepare(reader, image.f.binding, *workspace)); CHECK(reader.reads == before);
        ++positive;
    }
    else { CHECK(report.phase == Ext4ZeroTruncatePreviewPhase::failed); ++refusal; }
    if (!target_change && !head_change) CHECK(image.blocks == saved);
    workspace->~Workspace();
    return reader.reads;
}
void verify_refused(Image &image, usize payload_count = 4, usize home_count = 4)
{
    const auto current = homes(image);
    saved = image.blocks;
    CHECK(!Ext4ZeroTruncateJournalVerifier::verify(image.f.binding, records(image, payload_count), image.blocks[4],
        std::span<const Block>{current}.first(home_count), journal_scratch, journal_result));
    CHECK(!journal_result.complete && !journal_result.active_sequence);
    CHECK(image.blocks == saved); ++refusal;
}
void journal_corruption()
{
    for (usize block = 0; block < 8; ++block)
        for (usize byte : {usize{0}, usize{127}, usize{500}, usize{1023}})
        {
            Image image(3); journal(image, 15); image.blocks[first + block][byte] ^= 1; verify_refused(image);
        }
    for (usize at : {usize{0}, usize{4}, usize{16}, usize{28}, usize{40}, usize{48}, usize{64}, usize{80}})
    {
        Image image(3); journal(image, 0); image.blocks[first][at] ^= 1;
        stamp(image.blocks[first], 252, ~u32{0}); verify_refused(image);
    }
    for (usize at : {usize{8}, usize{12}, usize{20}, usize{48}, usize{52}, usize{56}, usize{60}, usize{700}})
    {
        Image image(3); journal(image, 0); image.blocks[first + 6][at] ^= 1;
        stamp(image.blocks[first + 6], 16, image.f.seed); verify_refused(image);
    }
    for (usize home = 0; home < 4; ++home)
    {
        Image image(3); journal(image, 0); image.blocks[plan.targets[home]][700] ^= 1; verify_refused(image);
    }
    for (usize count : {usize{0}, usize{1}, usize{3}, usize{5}})
    { Image image(3); journal(image, 0); verify_refused(image, count); }
    for (usize count : {usize{0}, usize{1}, usize{3}})
    { Image image(3); journal(image, 0); verify_refused(image, 4, count); }
    {
        Image image(3); journal(image, 0); image.f.binding.empty_log = true; verify_refused(image);
    }
    {
        Image image(3); journal(image, 0); ++image.f.binding.journal_sequence; verify_refused(image);
    }
    {
        Image image(3); journal(image, 0); image.f.binding.uuid[0] ^= 1; verify_refused(image);
    }
    {
        Image image(3); journal(image, 0); be32(image.blocks[first + 1], 12, 72);
        stamp(image.blocks[first + 1], 1020, image.f.seed); verify_refused(image);
    }
    {
        Image image(3); journal(image, 0); be32(image.blocks[first], 40,
            jbd2::feature_64bit | jbd2::feature_checksum_v3);
        stamp(image.blocks[first], 252, ~u32{0}); verify_refused(image);
    }
}
void ownership_corruption()
{
    for (bool direct : {false, true})
    {
        Image image(direct ? 1 : 3); image.duplicate(14, direct); journal(image, 15); preview(image, StatusCode::io_error);
    }
    {
        Image image(3); put32(image.inode(2), 60, 72); stamp_inode(image.inode(2), 2, image.f.seed);
        journal(image, 0); preview(image, StatusCode::io_error);
    }
    {
        Image image(3); image.blocks[4][0] |= 4; image.inode(3)[40] = 72; image.refresh();
        journal(image, 0); preview(image, StatusCode::unsupported);
    }
    {
        Image image(3); put32(image.inode(8), 60, 72); stamp_inode(image.inode(8), 8, image.f.seed);
        journal(image, 0); preview(image, StatusCode::unsupported);
    }
    {
        Image image(1);
        for (u32 number = 11; number <= 140; ++number)
            if (number != 13)
            {
                auto raw = image.inode(number); for (auto &byte : raw) byte = 0;
                put16(raw, 0, 0100644); put16(raw, 26, 1); put32(raw, 32, 0x80000);
                put16(raw, 40, 0xf30a); put16(raw, 44, 4); put16(raw, 128, 32);
                stamp_inode(raw, number, image.f.seed); image.blocks[4][(number - 1) / 8] |= u8(1U << ((number - 1) % 8));
            }
        image.refresh(); journal(image, 15); preview(image, StatusCode::no_memory);
    }
    { Image image(3); journal(image, 0); preview(image, StatusCode::io_error, 0, true); }
    { Image image(3); journal(image, 15); preview(image, StatusCode::io_error, 0, false, true); }
}
} // namespace

int main()
{
    for (u32 mask = 0; mask < 16; ++mask)
        for (u32 count : {1U, 3U, 16U})
        {
            Image image(count); journal(image, mask);
            const auto current = homes(image);
            CHECK(Ext4ZeroTruncateJournalVerifier::verify(image.f.binding, records(image), image.blocks[4],
                current, journal_scratch, journal_result));
            CHECK(journal_result.complete && journal_result.after_mask == mask);
            CHECK(journal_scratch.decoded.before == plan.before && journal_scratch.decoded.after == plan.after);
            ++positive;
            preview(image);
        }
    for (u32 mask = 0; mask < 16; ++mask)
    {
        Image image(16, true); journal(image, mask); preview(image);
    }
    {
        Image image(3); journal(image, 0);
        be32(image.blocks[first], 40, jbd2::feature_revoke | jbd2::feature_checksum_v3);
        stamp(image.blocks[first], 252, ~u32{0}); preview(image);
    }
    usize actual_reads{};
    { Image image(3); journal(image, 15); actual_reads = preview(image); }
    for (usize fault = 1; fault <= actual_reads; ++fault)
    {
        Image image(3); journal(image, 15); preview(image, StatusCode::io_error, fault); ++read_faults;
    }
    journal_corruption(); ownership_corruption();
    std::printf("PASS hosted zero-truncate committed preview positive=%zu refusal=%zu read_faults=%zu baseline_reads=%zu\n",
        positive, refusal, read_faults, actual_reads);
}
