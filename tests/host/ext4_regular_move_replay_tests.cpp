// Synthetic pure replay-validation model; no device or recovery I/O.
#include "ok/fs/ext4_regular_move_replay.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "regular move plan model:%d: %s\n", __LINE__, #condition); std::abort(); \
} } while (false)

namespace
{
using Block = Ext4RegularMoveBlock;
using namespace ext4_checked;
void put16(std::span<u8> bytes, usize at, u16 value)
{ bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
void put32(std::span<u8> bytes, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) bytes[at + byte] = u8(value >> (8 * byte)); }
usize slot(u32 number) { return ((number - 1) % 4) * 256; }

// Synthetic checksummed images exercise planning only. There is no device,
// fixture command, mounted VFS, journal emission, replay or publication here.
struct Fixture
{
    Ext4RecoveryInfo binding{};
    Block super{}, descriptors{}, from_directory{}, to_directory{};
    std::array<Block, 8> tables{};
    std::array<Block, 4> inode_bitmaps{}, block_bitmaps{};
    Ext4RegularMoveEmptyOrphanAuthorization orphan{};
    u32 from{}, to{}, source{}, seed{};
    static constexpr u32 from_block = 600, to_block = 601, file_block = 602;

    explicit Fixture(unsigned sharing = 0)
    {
        from = sharing == 0 ? 2 : sharing == 1 ? 11 : 13;
        to = sharing == 0 ? 11 : sharing == 1 ? 12 : 14;
        source = sharing == 2 ? 15 : 13;
        binding.layout.block_size = 1024; binding.layout.filesystem_blocks = 32768;
        binding.layout.journal_first_block = 1000; binding.layout.journal_blocks = 1024;
        binding.journal_inode = 8; binding.metadata_count = 16;
        binding.empty_log = true;
        for (usize index = 0; index < binding.uuid.size(); ++index) binding.uuid[index] = u8(index + 7);
        seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        orphan = {binding.uuid, 0, true};
        put32(super, 0, 8192); put32(super, 4, 32768); put32(super, 20, 1);
        put32(super, 32, 8192); put32(super, 40, 2048); put16(super, 56, 0xef53);
        put32(super, 84, 11); put16(super, 88, 256); put32(super, 92, 4);
        put32(super, 96, 0x42); put32(super, 100, 0x400); put32(super, 224, 8); super[373] = 1;
        for (usize index = 0; index < binding.uuid.size(); ++index) super[104 + index] = binding.uuid[index];
        for (u32 group = 0; group < 4; ++group)
        {
            const u32 begin = 1 + group * 8192;
            binding.metadata[group * 4] = {begin, 2};
            binding.metadata[group * 4 + 1] = {begin + 2, 1};
            binding.metadata[group * 4 + 2] = {begin + 3, 1};
            binding.metadata[group * 4 + 3] = {begin + 4, 512};
            put32(descriptors, group * 32, begin + 2);
            put32(descriptors, group * 32 + 4, begin + 3);
            put32(descriptors, group * 32 + 8, begin + 4);
            inode_bitmaps[group].fill(0xff); block_bitmaps[group].fill(0xff);
            refresh_bitmaps(group);
        }
        put32(super, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
        initialize_inode(from, true, from_block);
        initialize_inode(to, true, to_block);
        initialize_inode(source, false, file_block);
        initialize_directory(from_directory, from, 2);
        initialize_directory(to_directory, to, 2);
        usize source_offset = 24;
        if (from == 2)
        {
            put16(node(from), 26, 3);
            entry(from_directory, 24, to, 16, "inbox", 2);
            source_offset = 40;
        }
        entry(from_directory, source_offset, source, u16(1012 - source_offset), "seed", 1);
        refresh_inode(from); refresh_inode(to); refresh_inode(source);
        refresh_directory(true); refresh_directory(false);
    }
    static u32 table_block(u32 number) { return 5 + (number - 1) / 4; }
    Block &table(u32 number) { return tables[(number - 1) / 4]; }
    const Block &table(u32 number) const { return tables[(number - 1) / 4]; }
    std::span<u8> node(u32 number) { return std::span<u8>{table(number)}.subspan(slot(number), 256); }
    void initialize_inode(u32 number, bool directory, u32 physical)
    {
        auto inode = node(number);
        put16(inode, 0, directory ? 0040755 : 0100640); put16(inode, 2, 1000); put16(inode, 24, 1001);
        put32(inode, 4, 1024); put32(inode, 8, 1700000000); put32(inode, 12, 1700000001);
        put32(inode, 16, 1700000002); put16(inode, 26, directory ? 2 : 1);
        put32(inode, 28, 2); put32(inode, 32, 0x80000); put32(inode, 36, 5);
        put16(inode, 40, 0xf30a); put16(inode, 42, 1); put16(inode, 44, 4);
        put16(inode, 56, 1); put32(inode, 60, physical); put32(inode, 100, number + 100);
        put16(inode, 128, 32); put32(inode, 132, 4); put32(inode, 136, 8); put32(inode, 140, 12);
    }
    static void entry(Block &block, usize offset, u32 inode, u16 record, std::string_view name, u8 type)
    {
        for (usize at = offset; at < offset + record; ++at) block[at] = 0;
        put32(block, offset, inode); put16(block, offset + 4, record);
        block[offset + 6] = u8(name.size()); block[offset + 7] = type;
        for (usize at = 0; at < name.size(); ++at) block[offset + 8 + at] = u8(name[at]);
    }
    static void initialize_directory(Block &block, u32 number, u32 parent)
    {
        entry(block, 0, number, 12, ".", 2); entry(block, 12, parent, 12, "..", 2);
        entry(block, 24, 0, 988, {}, 0); put16(block, 1016, 12); block[1019] = 0xde;
    }
    void refresh_inode(u32 number)
    {
        auto inode = node(number); inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(inode_seed(seed, number, inode), inode);
        put16(inode, 124, u16(crc)); put16(inode, 130, u16(crc >> 16));
    }
    void refresh_directory(bool from_side)
    {
        const u32 number = from_side ? from : to;
        auto &directory = from_side ? from_directory : to_directory;
        put32(directory, 1020, jbd2::detail::crc32c(inode_seed(seed, number, node(number)),
            std::span<const u8>{directory}.first(1012)));
    }
    void refresh_bitmaps(u32 group)
    {
        const usize at = group * 32;
        put16(descriptors, at + 24, u16(jbd2::detail::crc32c(seed, block_bitmaps[group])));
        put16(descriptors, at + 26, u16(jbd2::detail::crc32c(seed, std::span<const u8>{inode_bitmaps[group]}.first(256))));
        put16(descriptors, at + 30, 0);
        put16(descriptors, at + 30, u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, encoded(group)),
            std::span<const u8>{descriptors}.subspan(at, 32))));
    }
    Ext4RegularMoveSource images() const
    {
        return {table_block(from), table_block(to), table_block(source), from_block, to_block,
            super, descriptors, table(from), table(to), table(source), from_directory, to_directory,
            inode_bitmaps, block_bitmaps, orphan};
    }
    Ext4RegularMoveUpdate update(std::string_view new_name = "note") const
    { return {from, to, source, from + 100, to + 100, source + 100, "seed", new_name, 1700000999, 123456789}; }
    Status plan(Ext4RegularMovePlan &output) const
    { return Ext4RegularMovePlanner::plan(binding, update(), images(), output); }
};

void big32(Block &bytes, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) bytes[at + byte] = u8(value >> (24 - byte * 8)); }

// This reuses the pinned planner's synthetic fixture, not filesystem tools.
struct Snapshot
{
    Fixture fixture;
    Ext4RegularMovePlan planned{};
    Block head{}, descriptor{};
    std::array<Block, 5> payloads{}, homes{};
    std::array<jbd2::ReplayAction, 5> actions{};
    static constexpr u32 sequence = 41;

    explicit Snapshot(unsigned sharing = 0, u32 features = 17) : fixture(sharing)
    {
        CHECK(fixture.plan(planned));
        big32(head, 0, jbd2::magic); big32(head, 4, 4); big32(head, 12, 1024);
        big32(head, 16, fixture.binding.layout.journal_blocks); big32(head, 20, 1);
        big32(head, 24, sequence); big32(head, 28, 1); big32(head, 40, features);
        for (usize at = 0; at < 16; ++at) head[48 + at] = fixture.binding.uuid[at];
        big32(head, 64, 1); head[80] = 4;
        big32(head, 252, jbd2::detail::checksum_zeroed(~u32{0}, head, 252));
        rebuild(planned);
        homes = planned.before;
    }
    jbd2::Codec codec() const
    {
        const auto opened = jbd2::Codec::open(head, fixture.binding.layout.journal_blocks,
            fixture.binding.uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
        CHECK(opened); return opened.value();
    }
    jbd2::ReplayPlan replay() const
    { return {std::span<const jbd2::ReplayAction>{actions}.first(planned.count), 1, sequence + 1,
              u32(planned.count + 3), 0, false, false, 0}; }
    Ext4RegularMoveReplayAncillary ancillary() const
    { return {fixture.super, fixture.descriptors, fixture.inode_bitmaps, fixture.block_bitmaps, fixture.orphan}; }
    void stamp_descriptor()
    { big32(descriptor, 1020, jbd2::detail::checksum_zeroed(fixture.seed, descriptor, 1020)); }
    void rebuild(const Ext4RegularMovePlan &images)
    {
        descriptor = {}; big32(descriptor, 0, jbd2::magic); big32(descriptor, 4, 1);
        big32(descriptor, 8, sequence);
        const std::array<u8, 4> seq{0, 0, 0, u8(sequence)};
        for (usize index = 0; index < images.count; ++index)
        {
            payloads[index] = images.after[index];
            const bool escape = jbd2::detail::be32(payloads[index], 0) == jbd2::magic;
            if (escape) big32(payloads[index], 0, 0);
            const u32 crc = jbd2::detail::crc32c(jbd2::detail::crc32c(fixture.seed, seq), payloads[index]);
            actions[index] = {images.targets[index], u32(index + 2), sequence, 0, crc, escape};
            const usize at = index ? 44 + (index - 1) * 16 : 12;
            big32(descriptor, at, images.targets[index]); big32(descriptor, at + 12, crc);
            big32(descriptor, at + 4, (index ? jbd2::flag_same_uuid : 0) |
                (escape ? jbd2::flag_escape : 0) | (index + 1 == images.count ? jbd2::flag_last_tag : 0));
        }
        CHECK(Ext4RegularMoveWitnessCodec::encode(fixture.update(), images, descriptor));
        stamp_descriptor();
    }
    Status validate(Ext4RegularMoveReplayScratch &scratch, Ext4RegularMoveReplayValidation &output) const
    {
        const auto parsed = replay(); const auto journal = codec();
        return Ext4RegularMoveReplayValidator::validate(fixture.binding, journal, parsed, descriptor,
            std::span<const Block>{payloads}.first(planned.count),
            std::span<const Block>{homes}.first(planned.count), ancillary(), scratch, output);
    }
};

void refused(const Snapshot &snapshot)
{
    Ext4RegularMoveReplayScratch scratch{}; Ext4RegularMoveReplayValidation output{}; output.count = 77;
    CHECK(!snapshot.validate(scratch, output)); CHECK(output.count == 0);
}

void mixtures(unsigned sharing)
{
    Snapshot snapshot(sharing); const auto inputs = snapshot.fixture.tables;
    for (unsigned mask = 0; mask < (1U << snapshot.planned.count); ++mask)
    {
        for (usize index = 0; index < snapshot.planned.count; ++index)
            snapshot.homes[index] = mask & (1U << index) ? snapshot.planned.after[index] : snapshot.planned.before[index];
        Ext4RegularMoveReplayScratch scratch{}; Ext4RegularMoveReplayValidation output{};
        CHECK(snapshot.validate(scratch, output)); CHECK(output.count == snapshot.planned.count);
        CHECK(output.from_parent == snapshot.fixture.from && output.to_parent == snapshot.fixture.to);
        CHECK(output.source == snapshot.fixture.source && output.from_generation == snapshot.fixture.from + 100);
        CHECK(output.to_generation == snapshot.fixture.to + 100 && output.source_generation == snapshot.fixture.source + 100);
        CHECK(output.all_homes_after == (mask + 1 == (1U << snapshot.planned.count)));
        for (usize index = 0; index < output.count; ++index)
        {
            CHECK(output.home_after[index] == bool(mask & (1U << index)));
            CHECK(scratch.canonical.targets[index] == snapshot.planned.targets[index]);
            CHECK(scratch.canonical.before[index] == snapshot.planned.before[index]);
            CHECK(scratch.canonical.after[index] == snapshot.planned.after[index]);
        }
    }
    CHECK(snapshot.fixture.tables == inputs);
    Snapshot optional_revoke(sharing, 16);
    Ext4RegularMoveReplayScratch scratch{}; Ext4RegularMoveReplayValidation output{};
    CHECK(optional_revoke.validate(scratch, output));
}

void metadata_tampering()
{
    // Outer descriptor CRC is repaired for every mutation except the CRC test.
    for (usize at : {usize{128}, usize{136}, usize{143}, usize{144}, usize{148}, usize{152},
        usize{156}, usize{160}, usize{164}, usize{168}, usize{172}, usize{200}, usize{240},
        usize{272}, usize{306}, usize{319}, usize{1019}})
    {
        Snapshot changed; changed.descriptor[at] ^= 1; changed.stamp_descriptor(); refused(changed);
    }
    Snapshot descriptor_crc; descriptor_crc.descriptor[1020] ^= 1; refused(descriptor_crc);
    Snapshot payload_crc; payload_crc.payloads[2][900] ^= 1; refused(payload_crc);
    Snapshot padding; padding.descriptor[28] = 1; padding.stamp_descriptor(); refused(padding);
    Snapshot bad_tag; big32(bad_tag.descriptor, 16, jbd2::flag_same_uuid);
    bad_tag.stamp_descriptor(); refused(bad_tag);
    Snapshot escaped; escaped.actions[0].restore_magic = true;
    big32(escaped.descriptor, 16, jbd2::flag_escape); escaped.stamp_descriptor(); refused(escaped);
    for (usize index = 0; index < 5; ++index)
    {
        Snapshot torn; torn.homes[index][900] ^= 1; refused(torn);
        Snapshot reordered;
        const auto temporary = reordered.actions[0]; reordered.actions[0] = reordered.actions[index];
        reordered.actions[index] = temporary;
        if (index) refused(reordered);
    }
    Snapshot duplicate; duplicate.actions[1].target_block = duplicate.actions[0].target_block; refused(duplicate);
    Snapshot journal_position; ++journal_position.actions[1].journal_block; refused(journal_position);
    Snapshot transaction_order; transaction_order.actions[2].transaction_order = 1; refused(transaction_order);
    Snapshot sequence; ++sequence.actions[0].sequence; refused(sequence);
    Snapshot full_permutation; auto permuted = full_permutation.planned;
    const auto from_before = permuted.before[0], from_after = permuted.after[0];
    const u32 from_target = permuted.targets[0];
    permuted.before[0] = permuted.before[1]; permuted.after[0] = permuted.after[1];
    permuted.targets[0] = permuted.targets[1];
    permuted.before[1] = from_before; permuted.after[1] = from_after; permuted.targets[1] = from_target;
    full_permutation.rebuild(permuted); full_permutation.homes = permuted.before; refused(full_permutation);
}

void canonical_semantic_tampering()
{
    // All payload/descriptor/witness checksums are valid and old bytes remain
    // exact. Only pure planner reproduction can reject these altered images.
    for (usize byte : {usize{2}, usize{26}, usize{60}, usize{100}, usize{900}})
    {
        Snapshot changed; auto malicious = changed.planned;
        const usize source_slot = slot(changed.fixture.source);
        usize source_home{};
        while (malicious.targets[source_home] != Fixture::table_block(changed.fixture.source)) ++source_home;
        malicious.after[source_home][byte == 900 ? byte : source_slot + byte] ^= 1;
        auto inode = std::span<u8>{malicious.after[source_home]}.subspan(source_slot, 256);
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(inode_seed(changed.fixture.seed, changed.fixture.source, inode), inode);
        put16(inode, 124, u16(crc)); put16(inode, 130, u16(crc >> 16));
        changed.rebuild(malicious); refused(changed);
    }
    Snapshot foreign_dotdot; auto malicious = foreign_dotdot.planned;
    put32(malicious.after[1], 12, 99);
    const usize to_home = 3;
    const auto parent_inode = std::span<const u8>{malicious.after[to_home]}.subspan(slot(foreign_dotdot.fixture.to), 256);
    put32(malicious.after[1], 1020, jbd2::detail::crc32c(
        inode_seed(foreign_dotdot.fixture.seed, foreign_dotdot.fixture.to, parent_inode),
        std::span<const u8>{malicious.after[1]}.first(1012)));
    foreign_dotdot.rebuild(malicious); refused(foreign_dotdot);
    for (u32 number : {u32{2}, u32{11}, u32{13}})
    {
        Snapshot stale; const usize index = number == 2 ? 2 : number == 11 ? 3 : 4;
        auto inode = std::span<u8>{stale.homes[index]}.subspan(slot(number), 256);
        put32(inode, 100, le32(inode, 100) + 1);
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(inode_seed(stale.fixture.seed, number, inode), inode);
        put16(inode, 124, u16(crc)); put16(inode, 130, u16(crc >> 16)); refused(stale);
    }
}

void ancillary_refusals()
{
    Snapshot uuid; uuid.fixture.binding.uuid[0] ^= 1;
    // Keep a valid codec for the original UUID to test the binding check.
    Snapshot original; const auto journal = original.codec(); const auto parsed = uuid.replay();
    Ext4RegularMoveReplayScratch scratch{}; Ext4RegularMoveReplayValidation output{};
    CHECK(!Ext4RegularMoveReplayValidator::validate(uuid.fixture.binding, journal, parsed, uuid.descriptor,
        uuid.payloads, uuid.homes, uuid.ancillary(), scratch, output));
    Snapshot orphan; orphan.fixture.orphan.independently_checked_empty = false; refused(orphan);
    Snapshot orphan_head; orphan_head.fixture.orphan.observed_head = 13; refused(orphan_head);
    Snapshot allocation;
    allocation.fixture.inode_bitmaps[0][(allocation.fixture.source - 1) / 8] &=
        u8(~(1U << ((allocation.fixture.source - 1) % 8)));
    allocation.fixture.refresh_bitmaps(0); refused(allocation);
    Snapshot directory_allocation;
    directory_allocation.fixture.block_bitmaps[0][(Fixture::to_block - 1) / 8] &=
        u8(~(1U << ((Fixture::to_block - 1) % 8)));
    directory_allocation.fixture.refresh_bitmaps(0); refused(directory_allocation);
    Snapshot descriptors; descriptors.fixture.descriptors[30] ^= 1; refused(descriptors);
    Snapshot superblock; superblock.fixture.super[1020] ^= 1; refused(superblock);
}

void escaped_payload()
{
    Snapshot snapshot;
    // Inode 1 is outside the three selected inodes. Its retained table prefix
    // forces legal JBD2 escaping without changing the planned move semantics.
    big32(snapshot.fixture.table(snapshot.fixture.from), 0, jbd2::magic);
    CHECK(snapshot.fixture.plan(snapshot.planned)); snapshot.rebuild(snapshot.planned);
    snapshot.homes = snapshot.planned.before; CHECK(snapshot.actions[2].restore_magic);
    Ext4RegularMoveReplayScratch scratch{}; Ext4RegularMoveReplayValidation output{};
    CHECK(snapshot.validate(scratch, output));
    CHECK(scratch.after[2] == snapshot.planned.after[2]);
}

void shape_and_alias_refusals()
{
    Snapshot snapshot; auto parsed = snapshot.replay(); const auto journal = snapshot.codec();
    Ext4RegularMoveReplayScratch scratch{}; Ext4RegularMoveReplayValidation output{};
    const auto validate = [&] {
        return Ext4RegularMoveReplayValidator::validate(snapshot.fixture.binding, journal, parsed,
            snapshot.descriptor, snapshot.payloads, snapshot.homes, snapshot.ancillary(), scratch, output);
    };
    for (unsigned kind = 0; kind < 7; ++kind)
    {
        parsed = snapshot.replay(); output.count = 77;
        if (kind == 0) parsed.committed_transactions = 2;
        if (kind == 1) parsed.empty_log = true;
        if (kind == 2) parsed.incomplete_tail = true;
        if (kind == 3) parsed.revoked_actions = 1;
        if (kind == 4) parsed.revoke_records = 1;
        if (kind == 5) ++parsed.next_sequence;
        if (kind == 6) ++parsed.scanned_blocks;
        CHECK(!validate() && !output.count);
    }
    parsed = snapshot.replay(); output.count = 77; scratch.after[0].fill(0x61);
    const auto preserved = scratch.after;
    CHECK(Ext4RegularMoveReplayValidator::validate(snapshot.fixture.binding, journal, parsed,
        snapshot.descriptor, scratch.after, snapshot.homes, snapshot.ancillary(), scratch, output).code() ==
        StatusCode::invalid_argument);
    CHECK(output.count == 77 && scratch.after == preserved);
    const Ext4RegularMoveReplayAncillary aliased{scratch.after[0], snapshot.fixture.descriptors,
        snapshot.fixture.inode_bitmaps, snapshot.fixture.block_bitmaps, snapshot.fixture.orphan};
    CHECK(Ext4RegularMoveReplayValidator::validate(snapshot.fixture.binding, journal, parsed,
        snapshot.descriptor, snapshot.payloads, snapshot.homes, aliased, scratch, output).code() ==
        StatusCode::invalid_argument);
    CHECK(output.count == 77 && scratch.after == preserved);
    std::array<Block, 6> excessive{};
    CHECK(Ext4RegularMoveReplayValidator::validate(snapshot.fixture.binding, journal, parsed,
        snapshot.descriptor, excessive, snapshot.homes, snapshot.ancillary(), scratch, output).code() ==
        StatusCode::invalid_argument);
    CHECK(output.count == 77 && scratch.after == preserved);
    // A different-family or absent record is terminal for this validator.
    snapshot.descriptor[130] = 'C'; snapshot.stamp_descriptor(); CHECK(!validate() && !output.count);
}
} // namespace

int main()
{
    mixtures(0); mixtures(1); mixtures(2); metadata_tampering(); canonical_semantic_tampering();
    ancillary_refusals(); escaped_payload(); shape_and_alias_refusals();
    std::puts("EXT4_REGULAR_MOVE_REPLAY_VALIDATION_MODEL_PASS mixtures=56");
}
