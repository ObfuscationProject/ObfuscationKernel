#include "ext4_zero_truncate_preview_test_support.hpp"
#include <ok/fs/ext4_zero_truncate_journal.hpp>
#include <cstdio>
#include <cstdlib>

using namespace zero_preview_fixture;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "zero truncate journal format model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
Ext4ZeroTruncatePlan planner{}, plan{}, changed{};
Ext4ZeroTruncateJournalFormatScratch scratch{};
Ext4ZeroTruncateJournalFormat formatted{}, sentinel{};
Ext4ZeroTruncateJournalScratch verification{};
Ext4ZeroTruncateJournalResult result{};
Block head{}, saved_head{};
std::array<Block, 4> actual{};
usize positive{}, refusal{}, mixtures{};

void be32(std::span<u8> bytes, usize at, u32 value)
{ for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
void stamp(Block &bytes, usize at, u32 seed)
{ be32(bytes, at, 0); be32(bytes, at, jbd2::detail::crc32c(seed, bytes)); }
void prepare(Image &image, u32 sequence = 70, bool escape = false, bool revoke_feature = false)
{
    u32 inode = 13;
    if (escape)
    {
        const auto from = image.inode(13); auto target = image.inode(14);
        for (usize at = 0; at < 256; ++at) target[at] = from[at];
        stamp_inode(target, 14, image.f.seed);
        image.blocks[4][1] = u8((image.blocks[4][1] & ~0x10U) | 0x20U);
        be32(image.blocks[8], 0, jbd2::magic); image.refresh(); inode = 14;
    }
    const Ext4ZeroTruncateUpdate update{inode, 9, ext4_checked::le32(image.inode(inode), 4), 1700000111, 765432100};
    const Ext4ZeroTruncateSource source{image.f.group, 8, image.blocks[1], image.blocks[2],
        image.blocks[image.f.group ? 8195 : 3], image.blocks[4], image.blocks[8]};
    CHECK(Ext4ZeroTruncatePlanner::plan_off_stack(image.f.binding, update, source, planner, plan));
    head = {};
    be32(head, 0, jbd2::magic); be32(head, 4, u32(jbd2::RecordType::superblock_v2));
    be32(head, 12, 1024); be32(head, 16, 1024); be32(head, 20, 1);
    be32(head, 24, sequence); be32(head, 40, jbd2::feature_checksum_v3 | (revoke_feature ? jbd2::feature_revoke : 0));
    for (usize at = 0; at < image.f.binding.uuid.size(); ++at) head[48 + at] = image.f.binding.uuid[at];
    be32(head, 64, 1); head[80] = 4; stamp(head, 252, ~u32{0});
    image.f.binding.journal_sequence = sequence; saved_head = head;
}
void positive_format(Image &image, bool escaped = false)
{
    CHECK(Ext4ZeroTruncateJournalFormatter::format(image.f.binding, plan, head, image.blocks[4], scratch, formatted));
    CHECK(formatted.ready && formatted.active_sequence == image.f.binding.journal_sequence + 1 &&
        formatted.next_sequence == image.f.binding.journal_sequence + 2);
    CHECK(head == saved_head && jbd2::detail::all_zero(formatted.end));
    CHECK(jbd2::detail::be32(formatted.commit, 52) == 1700000111 && jbd2::detail::be32(formatted.commit, 56) == 765432100);
    CHECK(jbd2::detail::be32(formatted.empty_head, 24) == formatted.next_sequence && !jbd2::detail::be32(formatted.empty_head, 28));
    CHECK(bool(jbd2::detail::be32(formatted.descriptor, 16) & jbd2::flag_escape) == escaped);
    for (usize home = 0; home < 4; ++home)
    {
        Block payload = formatted.payloads[home];
        const usize at = home ? 44 + (home - 1) * 16 : 12;
        CHECK(jbd2::detail::be32(formatted.descriptor, at) == plan.targets[home]);
        if (jbd2::detail::be32(formatted.descriptor, at + 4) & jbd2::flag_escape) be32(payload, 0, jbd2::magic);
        CHECK(payload == plan.after[home]);
    }
    auto binding = image.f.binding; binding.empty_log = false; binding.journal_sequence = formatted.active_sequence;
    const Ext4ZeroTruncateJournalRecords records{formatted.head, formatted.descriptor, formatted.commit, formatted.end, formatted.payloads};
    for (u32 mask = 0; mask < 16; ++mask)
    {
        for (usize home = 0; home < 4; ++home) actual[home] = mask & (1U << home) ? plan.after[home] : plan.before[home];
        CHECK(Ext4ZeroTruncateJournalVerifier::verify(binding, records, image.blocks[4], actual, verification, result));
        CHECK(result.complete && result.after_mask == mask && verification.decoded.before == plan.before &&
            verification.decoded.after == plan.after && verification.decoded.released_mapping == plan.released_mapping);
        ++positive; ++mixtures;
    }
    ++positive;
}
void rejected(Image &image, const Ext4RecoveryInfo &binding, const Ext4ZeroTruncatePlan &input)
{
    formatted.head.fill(0xa5); formatted.ready = true;
    CHECK(!Ext4ZeroTruncateJournalFormatter::format(binding, input, head, image.blocks[4], scratch, formatted));
    CHECK(!formatted.ready && !formatted.active_sequence && !formatted.next_sequence &&
        jbd2::detail::all_zero(formatted.head) && jbd2::detail::all_zero(formatted.descriptor));
    ++refusal;
}
void negative_models()
{
    for (usize kind = 0; kind < 11; ++kind)
    {
        Image image(3); prepare(image); changed = plan;
        if (kind == 0) ++changed.inode;
        if (kind == 1) ++changed.generation;
        if (kind == 2) ++changed.old_size;
        if (kind == 3) ++changed.group;
        if (kind == 4) --changed.released_count;
        if (kind == 5) ++changed.released_mapping[15];
        if (kind == 6) ++changed.targets[0];
        if (kind == 7) changed.before[0][700] ^= 1;
        if (kind == 8) changed.after[0][700] ^= 1;
        if (kind == 9) changed.count = 3;
        if (kind == 10) changed.noop = true;
        rejected(image, image.f.binding, changed);
    }
    for (usize kind = 0; kind < 10; ++kind)
    {
        Image image(3); prepare(image); auto binding = image.f.binding;
        if (kind == 0) binding.needs_recovery = false;
        if (kind == 1) binding.empty_log = false;
        if (kind == 2) ++binding.journal_sequence;
        if (kind == 3) binding.uuid[0] ^= 1;
        if (kind == 4) binding.layout.block_size = 4096;
        if (kind == 5) --binding.layout.journal_blocks;
        if (kind == 6) binding.metadata_count = 0;
        if (kind == 7) be32(head, 28, 1);
        if (kind == 8) be32(head, 40, jbd2::feature_checksum_v3 | jbd2::feature_64bit);
        if (kind == 9) head[700] ^= 1;
        if (kind == 7 || kind == 8) stamp(head, 252, ~u32{0});
        rejected(image, binding, plan);
    }
    for (u32 sequence : {0U, 0xfffffffdU, 0xfffffffeU, 0xffffffffU})
    { Image image(3); prepare(image, sequence); rejected(image, image.f.binding, plan); }
    {
        Image image(0); prepare(image); rejected(image, image.f.binding, plan);
    }
    {
        Image image(3); prepare(image); image.blocks[4][1] ^= 0x10; rejected(image, image.f.binding, plan);
    }
    // Alias rejection preserves every input byte instead of clearing it.
    {
        Image image(3); prepare(image);
        formatted = {}; formatted.head = head; sentinel = formatted;
        CHECK(!Ext4ZeroTruncateJournalFormatter::format(image.f.binding, plan, formatted.head, image.blocks[4], scratch, formatted));
        CHECK(formatted.head == sentinel.head && formatted.payloads == sentinel.payloads); ++refusal;
        const auto *before = reinterpret_cast<const u8 *>(&scratch);
        const auto checksum = jbd2::detail::crc32c(~u32{0}, std::span<const u8>{before, sizeof(scratch)});
        CHECK(!Ext4ZeroTruncateJournalFormatter::format(image.f.binding, plan, scratch.verification.record,
            image.blocks[4], scratch, formatted));
        CHECK(jbd2::detail::crc32c(~u32{0}, std::span<const u8>{before, sizeof(scratch)}) == checksum); ++refusal;
    }
}
} // namespace

int main()
{
    for (bool cross : {false, true})
        for (u32 count : {1U, 3U, 16U})
            for (bool escape : {false, true})
                for (bool revoke : {false, true})
                { Image image(count, cross); prepare(image, 70, escape, revoke); positive_format(image, escape); }
    for (u32 sequence : {1U, 0xfffffffcU})
    { Image image(3); prepare(image, sequence); positive_format(image); }
    negative_models();
    std::printf("PASS hosted zero-truncate canonical journal format positive=%zu refusal=%zu mixtures=%zu\n", positive, refusal, mixtures);
    return 0;
}
