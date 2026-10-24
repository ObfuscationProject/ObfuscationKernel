#include "ok/fs/ext4_regular_move_transaction.hpp"
#include <memory>
#include <type_traits>
#include <vector>
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs;

usize checks{};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "regular move transaction source model:%d: %s\n", __LINE__, #condition); std::abort(); \
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

// Retained synthetic planner-image construction, extended below with an
// in-memory block model. No image fixture command or real backend is needed.
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

void big32(std::span<u8> bytes, usize at, u32 value)
{ for (usize byte = 0; byte < 4; ++byte) bytes[at + byte] = u8(value >> (24 - 8 * byte)); }
struct JournalFixture : Fixture
{
    Block head{};
    explicit JournalFixture(unsigned sharing = 0, u32 features = jbd2::feature_checksum_v3) : Fixture(sharing)
    {
        binding.needs_recovery = true; binding.journal_sequence = 51;
        put32(super, 96, 0x46); put32(super, 76, 1); put16(super, 58, 1); put32(super, 36, 8192);
        put32(super, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
        initialize_inode(8, false, 1000);
        auto journal = node(8);
        put16(journal, 0, 0100600); put16(journal, 2, 0); put16(journal, 24, 0);
        put32(journal, 4, 1024 * 1024); put32(journal, 28, 2048); put16(journal, 56, 1024);
        refresh_inode(8);
        big32(head, 0, jbd2::magic); big32(head, 4, u32(jbd2::RecordType::superblock_v2));
        big32(head, 12, 1024); big32(head, 16, 1024); big32(head, 20, 1);
        big32(head, 24, binding.journal_sequence); big32(head, 40, features); big32(head, 64, 1); head[80] = 4;
        for (usize byte = 0; byte < binding.uuid.size(); ++byte) head[48 + byte] = binding.uuid[byte];
        big32(head, 252, jbd2::detail::crc32c(~u32{0}, head));
    }
};

// Caller-owned model media has separate volatile/durable bytes. Faults are
// injected at every publication boundary; no replay or home writes are made.
struct Disk final : driver::BlockDevice
{
    struct Event { char operation{}; u64 sector{}; };
    std::vector<u8> live, durable;
    std::vector<Event> events;
    static constexpr u64 first{7};
    driver::BlockGeometry shape{};
    usize reads{}, writes{}, flushes{}, fail_read{}, fail_write{}, fail_flush{}, partial{}, geometry_read{}, geometry_write{}, geometry_flush{};
    bool after{};
    explicit Disk(const JournalFixture &fixture)
        : live(usize(first) * 512 + 32768 * 1024), durable(live)
    {
        shape = {live.size() / 512, 512, true};
        put(1, fixture.super); put(2, fixture.descriptors); put(1000, fixture.head);
        put(Fixture::from_block, fixture.from_directory); put(Fixture::to_block, fixture.to_directory);
        for (usize index = 0; index < fixture.tables.size(); ++index) put(u32(index + 5), fixture.tables[index]);
        for (usize group = 0; group < 4; ++group)
        {
            put(ext4_checked::le32(fixture.descriptors, group * 32), fixture.block_bitmaps[group]);
            put(ext4_checked::le32(fixture.descriptors, group * 32 + 4), fixture.inode_bitmaps[group]);
        }
        // File bytes are retained even when an empty-file variation has no extent.
        Block data{}; data.fill(0x42); put(Fixture::file_block, data); durable = live;
    }
    driver::BlockGeometry geometry() const override { return shape; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads; events.push_back({'r', sector});
        if (reads == fail_read) return Status::io_error("regular move model read cut");
        if (sector > live.size() / 512 || out.size() > live.size() - usize(sector) * 512)
            return Status::invalid_argument("regular move model read bounds");
        for (usize byte = 0; byte < out.size(); ++byte) out[byte] = std::byte(live[usize(sector) * 512 + byte]);
        if (reads == geometry_read) --shape.block_count;
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> in) override
    {
        ++writes; events.push_back({'w', sector});
        if (sector > live.size() / 512 || in.size() > live.size() - usize(sector) * 512)
            return Status::invalid_argument("regular move model write bounds");
        const bool fail = writes == fail_write;
        const usize bytes = fail && !after ? (partial < in.size() ? partial : in.size()) : in.size();
        for (usize byte = 0; byte < bytes; ++byte) live[usize(sector) * 512 + byte] = u8(in[byte]);
        if (writes == geometry_write) --shape.block_count;
        return fail ? Status::io_error("regular move model write cut") : Status{};
    }
    Status flush() override
    {
        ++flushes; events.push_back({'f', 0});
        const bool fail = flushes == fail_flush;
        if (!fail || after) durable = live;
        if (flushes == geometry_flush) --shape.block_count;
        return fail ? Status::io_error("regular move model flush cut") : Status{};
    }
    Ext4RecoveryBounds bounds() const { return {first, 32768 * 2}; }
    void put(u32 block, const Block &bytes)
    { for (usize byte = 0; byte < bytes.size(); ++byte) live[usize(first) * 512 + usize(block) * 1024 + byte] = bytes[byte]; }
    Block get(u32 block, bool persisted = false) const
    {
        const auto &image = persisted ? durable : live; Block result{};
        for (usize byte = 0; byte < result.size(); ++byte) result[byte] = image[usize(first) * 512 + usize(block) * 1024 + byte];
        return result;
    }
    void mutate(u32 block, usize byte = 777)
    { live[usize(first) * 512 + usize(block) * 1024 + byte] ^= 1; }
};

void journal_construction(unsigned sharing, u32 features)
{
    JournalFixture fixture(sharing, features);
    // Unselected inode-table padding can begin with JBD2 magic. Selected slots
    // remain checked; the payload must escape and structurally restore this.
    if (slot(fixture.from)) big32(fixture.table(fixture.from), 0, jbd2::magic);
    const auto old_tables = fixture.tables; const auto old_from = fixture.from_directory, old_to = fixture.to_directory;
    auto scratch = std::make_unique<Ext4RegularMoveJournalScratch>();
    auto journal = std::make_unique<Ext4RegularMoveJournalPlan>();
    CHECK(Ext4RegularMoveJournalPlanner::plan(fixture.binding, fixture.update(), fixture.images(), fixture.head, *scratch, *journal));
    const auto &canonical = scratch->regular_move_plan;
    CHECK(journal->ready && journal->count == 5 - sharing && journal->sequence == 52);
    // Shared tables must contain all three distinct slot edits exactly once.
    for (u32 number : {fixture.from, fixture.to, fixture.source})
    {
        usize found = canonical.count;
        for (usize home = 0; home < canonical.count; ++home)
            if (canonical.targets[home] == Fixture::table_block(number)) found = home;
        CHECK(found < canonical.count);
        const auto after = std::span<const u8>{canonical.after[found]}.subspan(slot(number), 256);
        const auto before = std::span<const u8>{canonical.before[found]}.subspan(slot(number), 256);
        CHECK(ext4_checked::inode_checksum(fixture.seed, number, after));
        CHECK(le32(after, 100) == number + 100 && le16(after, 26) == le16(before, 26));
        CHECK(le32(after, 12) == fixture.update().seconds && le32(after, 36) == 6);
        CHECK(le32(after, 16) == (number == fixture.source ? le32(before, 16) : fixture.update().seconds));
    }
    CHECK(Ext4RegularMoveWitnessCodec::present(journal->descriptor));
    CHECK(scratch->witness_before == canonical.before);
    const auto codec = jbd2::Codec::open(journal->journal_head, 1024, fixture.binding.uuid,
        jbd2::DescriptorUuidPolicy::linux_internal_zero); CHECK(codec);
    CHECK(codec.value().superblock().start_block == 1 && codec.value().superblock().features == features);
    usize tags{};
    CHECK(codec.value().descriptor(journal->descriptor, 52, [&](const jbd2::Tag &tag) -> Status {
        CHECK(tag.target_block == canonical.targets[tags]);
        CHECK(tag.restore_magic == (jbd2::detail::be32(canonical.after[tags], 0) == jbd2::magic));
        if (tag.restore_magic) CHECK(jbd2::detail::be32(journal->payloads[tags], 0) == 0);
        CHECK(codec.value().payload(journal->payloads[tags], 52, tag)); ++tags; return {};
    }));
    CHECK(tags == journal->count);
    const auto commit = codec.value().commit(journal->commit, 52); CHECK(commit);
    CHECK(commit.value().seconds == fixture.update().seconds && commit.value().nanoseconds == fixture.update().nanoseconds);
    CHECK(jbd2::detail::all_zero(journal->end));
    CHECK(fixture.tables == old_tables && fixture.from_directory == old_from && fixture.to_directory == old_to);
    // Names in the descriptor and canonical plan survive caller buffers.
    char old_name[]{'s','e','e','d'}, new_name[]{'n','o','t','e'};
    auto update = fixture.update(); update.old_name = {old_name, 4}; update.new_name = {new_name, 4};
    CHECK(Ext4RegularMoveJournalPlanner::plan(fixture.binding, update, fixture.images(), fixture.head, *scratch, *journal));
    old_name[0] = 'x'; new_name[0] = 'y';
    CHECK(scratch->decoded_witness.update.old_name == "seed" && scratch->decoded_witness.update.new_name == "note");
    // Refused aliases preserve the old head/output before any reset.
    const auto saved = journal->journal_head;
    CHECK(Ext4RegularMoveJournalPlanner::plan(fixture.binding, fixture.update(), fixture.images(), journal->journal_head,
        *scratch, *journal).code() == StatusCode::invalid_argument);
    CHECK(journal->journal_head == saved);
    auto source = fixture.images();
    const Ext4RegularMoveSource aliased{source.from_parent_table_block, source.to_parent_table_block, source.source_table_block,
        source.from_directory_block, source.to_directory_block, source.superblock, source.descriptors,
        source.from_parent_table, source.to_parent_table, source.source_table, scratch->regular_move_plan.before[0],
        source.to_directory, source.inode_bitmaps, source.block_bitmaps, source.orphan};
    const auto prior = scratch->regular_move_plan.before[0];
    CHECK(Ext4RegularMoveJournalPlanner::plan(fixture.binding, fixture.update(), aliased, fixture.head, *scratch, *journal).code() == StatusCode::invalid_argument);
    CHECK(scratch->regular_move_plan.before[0] == prior);
}

void homes_unchanged(const Disk &disk, const Ext4RegularMovePlan &plan)
{
    for (usize index = 0; index < plan.count; ++index)
    { CHECK(disk.get(plan.targets[index]) == plan.before[index]); CHECK(disk.get(plan.targets[index], true) == plan.before[index]); }
    Block original_data{}; original_data.fill(0x42);
    CHECK(disk.get(Fixture::file_block) == original_data && disk.get(Fixture::file_block, true) == original_data);
}

void successful(unsigned sharing, u32 features, bool empty = false)
{
    JournalFixture fixture(sharing, features);
    if (empty)
    {
        auto inode = fixture.node(fixture.source); put32(inode, 4, 0); put32(inode, 28, 0); put16(inode, 42, 0);
        for (usize byte = 52; byte < 100; ++byte) inode[byte] = 0;
        fixture.refresh_inode(fixture.source);
    }
    Disk disk(fixture); const auto original = disk.live;
    auto transaction = std::make_unique<Ext4RegularMoveTransaction>();
    CHECK(transaction->prepare(disk, disk.bounds(), fixture.update()));
    CHECK(!disk.writes && !disk.flushes && disk.live == original && disk.durable == original);
    CHECK(transaction->canonical_plan().count == 5 - sharing && transaction->canonical_plan().retained_data_count == (empty ? 0U : 1U));
    std::array<u64, 6> blocks{}; std::array<Block, 6> images{}; usize count{};
    CHECK(transaction->logical_images(blocks, images, count)); CHECK(count == 6 - sharing);
    CHECK(blocks[0] == 1000 && images[0] == transaction->journal_plan().journal_head);
    for (usize index = 1; index < count; ++index)
    {
        CHECK(blocks[index] == transaction->canonical_plan().targets[index - 1]);
        CHECK(images[index] == transaction->canonical_plan().after[index - 1]);
        CHECK(*transaction->original_preimage(blocks[index]) == disk.get(u32(blocks[index])));
    }
    const usize first_event = disk.events.size();
    CHECK(transaction->publish_prepared()); CHECK(transaction->report().published());
    CHECK(disk.writes == transaction->canonical_plan().count + 4 && disk.flushes == 4 && disk.live == disk.durable);
    const auto &report = transaction->report();
    CHECK(report.write_attempts == disk.writes && report.completed_writes == disk.writes);
    CHECK(report.flush_attempts == 4 && report.completed_flushes == 4);
    CHECK(report.payloads_durable && report.commit_record_durable && report.recovery_bit_durable && report.publication_durable);
    homes_unchanged(disk, transaction->canonical_plan());
    std::vector<Disk::Event> events;
    for (usize index = first_event; index < disk.events.size(); ++index)
        if (disk.events[index].operation != 'r') events.push_back(disk.events[index]);
    const usize homes = transaction->canonical_plan().count;
    CHECK(events.size() == homes + 8 && events[0].operation == 'f');
    CHECK(events[1].operation == 'w' && events[1].sector == disk.first + 1001 * 2);
    for (usize index = 0; index < homes; ++index)
        CHECK(events[index + 2].operation == 'w' && events[index + 2].sector == disk.first + (1002 + index) * 2);
    CHECK(events[homes + 2].operation == 'w' && events[homes + 2].sector == disk.first + (1003 + homes) * 2);
    CHECK(events[homes + 3].operation == 'f');
    CHECK(events[homes + 4].operation == 'w' && events[homes + 4].sector == disk.first + (1002 + homes) * 2);
    CHECK(events[homes + 5].operation == 'f');
    CHECK(events[homes + 6].operation == 'w' && events[homes + 6].sector == disk.first + 1000 * 2);
    CHECK(events[homes + 7].operation == 'f');
    CHECK(transaction->matches_published());
    const usize writes = disk.writes, flushes = disk.flushes;
    CHECK(!transaction->publish_prepared() && !transaction->prepare(disk, disk.bounds(), fixture.update()));
    CHECK(disk.writes == writes && disk.flushes == flushes);
    disk.mutate(1001); CHECK(!transaction->matches_published()); CHECK(transaction->report().phase == Ext4TransactionPhase::failed);
}

void exact_preimages(unsigned sharing)
{
    JournalFixture fixture(sharing);
    auto scratch = std::make_unique<Ext4RegularMoveJournalScratch>(); Ext4RegularMoveJournalPlan journal{};
    CHECK(Ext4RegularMoveJournalPlanner::plan(fixture.binding, fixture.update(), fixture.images(), fixture.head, *scratch, journal));
    std::vector<u32> targets{1, 2, 1000, Fixture::table_block(8)};
    for (usize group = 0; group < 4; ++group)
    {
        targets.push_back(ext4_checked::le32(fixture.descriptors, group * 32));
        targets.push_back(ext4_checked::le32(fixture.descriptors, group * 32 + 4));
    }
    for (usize index = 0; index < journal.count; ++index) targets.push_back(journal.targets[index]);
    for (u32 target : targets)
    {
        Disk disk(fixture); auto transaction = std::make_unique<Ext4RegularMoveTransaction>();
        CHECK(transaction->prepare(disk, disk.bounds(), fixture.update())); disk.mutate(target);
        const auto changed = disk.live, persisted = disk.durable;
        CHECK(!transaction->publish_prepared()); CHECK(!disk.writes && !disk.flushes);
        CHECK(disk.live == changed && disk.durable == persisted);
        CHECK(transaction->report().failed_phase == Ext4TransactionPhase::preflight && transaction->report().phase == Ext4TransactionPhase::failed);
        CHECK(!transaction->publish_prepared()); CHECK(!disk.writes && !disk.flushes);
    }
}

void cut(unsigned sharing, usize write, usize flush, bool after, usize bytes = 0)
{
    JournalFixture fixture(sharing); Disk disk(fixture);
    auto transaction = std::make_unique<Ext4RegularMoveTransaction>();
    CHECK(transaction->prepare(disk, disk.bounds(), fixture.update()));
    const auto plan = transaction->canonical_plan();
    disk.fail_write = write; disk.fail_flush = flush; disk.after = after; disk.partial = bytes;
    CHECK(!transaction->publish_prepared());
    CHECK(transaction->report().phase == Ext4TransactionPhase::failed && !transaction->report().published());
    CHECK(transaction->report().write_attempts == disk.writes && transaction->report().flush_attempts == disk.flushes);
    CHECK(transaction->report().completed_writes == disk.writes - (write ? 1 : 0));
    CHECK(transaction->report().completed_flushes == disk.flushes - (flush ? 1 : 0));
    homes_unchanged(disk, plan);
    const usize writes = disk.writes, flushes = disk.flushes;
    CHECK(!transaction->publish_prepared() && !transaction->matches_published());
    CHECK(disk.writes == writes && disk.flushes == flushes);
    // No home publication is inferred from a failed journal call. The durable
    // head may be active after a flush returns failure *after* persistence.
    const auto durable_head = disk.get(1000, true);
    CHECK(jbd2::detail::be32(durable_head, 28) == (flush == 4 && after ? 1U : 0U));
    CHECK(transaction->report().publication_durable == false);
    CHECK(transaction->report().commit_record_durable == (write == plan.count + 4 || flush == 4));
}

void preparation_refusals()
{
    JournalFixture fixture;
    for (unsigned variant = 0; variant < 8; ++variant)
    {
        JournalFixture broken; auto update = broken.update();
        if (variant == 0) ++update.source_generation;
        if (variant == 1) ++update.from_generation;
        if (variant == 2) ++update.to_generation;
        if (variant == 3) update.old_name = "missing";
        if (variant == 4) update.new_name = ".";
        if (variant == 5) { put16(broken.node(broken.source), 26, 2); broken.refresh_inode(broken.source); }
        if (variant == 6) { Fixture::entry(broken.to_directory, 24, broken.source, 988, "note", 1); broken.refresh_directory(false); }
        if (variant == 7) broken.from_directory[1020] ^= 1;
        Disk disk(broken); auto transaction = std::make_unique<Ext4RegularMoveTransaction>();
        CHECK(!transaction->prepare(disk, disk.bounds(), update)); CHECK(!disk.writes && !disk.flushes);
    }
    for (unsigned variant = 0; variant < 4; ++variant)
    {
        Disk disk(fixture); auto transaction = std::make_unique<Ext4RegularMoveTransaction>();
        if (variant == 0) disk.shape.writable = false;
        if (variant == 1) disk.geometry_read = 1;
        if (variant == 2) disk.fail_read = 1;
        if (variant < 3) CHECK(!transaction->prepare(disk, disk.bounds(), fixture.update()));
        else
        {
            CHECK(transaction->prepare(disk, disk.bounds(), fixture.update())); --disk.shape.block_count;
            CHECK(!transaction->publish_prepared());
        }
        CHECK(!disk.writes && !disk.flushes);
    }
    for (bool write_change : {false, true})
    {
        Disk changed(fixture); auto transaction = std::make_unique<Ext4RegularMoveTransaction>();
        CHECK(transaction->prepare(changed, changed.bounds(), fixture.update()));
        if (write_change) changed.geometry_write = 1; else changed.geometry_flush = 1;
        CHECK(!transaction->publish_prepared());
        CHECK(transaction->report().phase == Ext4TransactionPhase::failed && !transaction->report().published());
        CHECK(changed.writes == (write_change ? 1U : 0U) && changed.flushes == 1);
        CHECK(transaction->report().completed_writes == changed.writes && transaction->report().completed_flushes == 1);
        homes_unchanged(changed, transaction->canonical_plan());
    }
    Disk disk(fixture); auto transaction = std::make_unique<Ext4RegularMoveTransaction>();
    CHECK(transaction->prepare(disk, disk.bounds(), fixture.update())); disk.fail_read = disk.reads + 1;
    CHECK(!transaction->publish_prepared()); CHECK(!disk.writes && !disk.flushes);
    auto source = fixture.images(); auto scratch = std::make_unique<Ext4RegularMoveJournalScratch>(); Ext4RegularMoveJournalPlan output{};
    // Sequence wrap/ring reuse and a clean binding are refused before readiness.
    fixture.binding.needs_recovery = false;
    CHECK(!Ext4RegularMoveJournalPlanner::plan(fixture.binding, fixture.update(), source, fixture.head, *scratch, output) && !output.ready);
    fixture.binding.needs_recovery = true; fixture.binding.journal_sequence = 0xfffffffdU;
    big32(fixture.head, 24, fixture.binding.journal_sequence); big32(fixture.head, 252, 0);
    big32(fixture.head, 252, jbd2::detail::crc32c(~u32{0}, fixture.head));
    CHECK(!Ext4RegularMoveJournalPlanner::plan(fixture.binding, fixture.update(), source, fixture.head, *scratch, output) && !output.ready);
}
} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<Ext4RegularMoveTransaction> && !std::is_move_constructible_v<Ext4RegularMoveTransaction>);
    for (unsigned sharing = 0; sharing < 3; ++sharing)
    {
        for (u32 features : {jbd2::feature_checksum_v3, jbd2::feature_checksum_v3 | jbd2::feature_revoke})
        { journal_construction(sharing, features); successful(sharing, features); }
        exact_preimages(sharing);
        const usize homes = 5 - sharing;
        for (usize write = 1; write <= homes + 4; ++write)
        {
            cut(sharing, write, 0, false); cut(sharing, write, 0, true);
            for (usize bytes : {1U, 28U, 64U, 256U, 512U, 1023U}) cut(sharing, write, 0, false, bytes);
        }
        for (usize flush = 1; flush <= 4; ++flush)
        { cut(sharing, 0, flush, false); cut(sharing, 0, flush, true); }
    }
    successful(2, jbd2::feature_checksum_v3, true); preparation_refusals();
    std::printf("EXT4_REGULAR_MOVE_TRANSACTION_SOURCE_MODEL_PASS checks=%zu\n", checks);
}
