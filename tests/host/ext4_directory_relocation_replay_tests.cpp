#include "ext4_completion_support.hpp"
#include "ext4_rename_relocation_support.hpp"
#include "../../src/fs/ext4_rename_plan.hpp"
#include <ok/fs/ext4_completion.hpp>
#include <algorithm>
#include <fstream>
#include <iterator>

using namespace ok;
using namespace ok::fs;
using namespace fixture;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL directory replay:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace
{
struct Disk final : driver::BlockDevice
{
    explicit Disk(const Bytes &image) : live(image), durable(image) {}
    Bytes live, durable;
    usize reads{}, writes{}, flushes{}, fail_read{}, fail_write{}, fail_flush{};
    bool partial_failure{};
    driver::BlockGeometry geometry() const override { return {live.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (reads == fail_read) return Status::io_error("injected directory replay read");
        if (first >= live.size() / 512 || out.size() % 512 || out.size() / 512 > live.size() / 512 - first)
            return Status::io_error("directory replay read bounds");
        for (usize index = 0; index < out.size(); ++index) out[index] = std::byte(live[first * 512 + index]);
        return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        ++writes;
        if (first >= live.size() / 512 || input.size() % 512 || input.size() / 512 > live.size() / 512 - first)
            return Status::io_error("directory replay write bounds");
        if (writes == fail_write && !partial_failure) return Status::io_error("injected directory replay write");
        const usize count = writes == fail_write ? input.size() / 2 : input.size();
        for (usize index = 0; index < count; ++index)
            live[first * 512 + index] = std::to_integer<u8>(input[index]);
        return writes == fail_write ? Status::io_error("injected torn directory replay write") : Status{};
    }
    Status flush() override
    {
        ++flushes;
        if (flushes == fail_flush) return Status::io_error("injected directory replay flush");
        durable = live;
        return {};
    }
};

struct Transaction
{
    Bytes published, completed;
    u32 parent_table{}, source_table{}, directory_block{};
    usize journal_offset{};
    u32 sequence{};
};

void tag(Bytes &bytes, usize at, u32 target, u32 flags, usize payload, u32 sequence)
{
    big32(bytes, at, target); big32(bytes, at + 4, flags); big32(bytes, at + 8, 0);
    const std::array<u8, 4> seq{u8(sequence >> 24), u8(sequence >> 16), u8(sequence >> 8), u8(sequence)};
    const u32 sum = crc(ext2fs_crc32c_le(seed(bytes), seq.data(), seq.size()), bytes, payload, 1024);
    big32(bytes, at + 12, sum);
}

Transaction make_transaction(const Bytes &clean)
{
    Transaction t{clean, clean};
    auto &b = t.published;
    t.journal_offset = journal(b);
    t.sequence = be32(b, t.journal_offset + 24) + 1;
    const usize parent_at = inode_offset(b, 2), source_at = inode_offset(b, 12);
    t.parent_table = u32(parent_at / 1024); t.source_table = u32(source_at / 1024);
    t.directory_block = r32(b, parent_at + 60);
    CHECK(t.parent_table != t.source_table && t.directory_block == 517);
    Disk readonly(clean); Ext4RecoveryBinding admitted;
    CHECK(admitted.inspect(readonly, {0, clean.size() / 512}));
    std::array<u8, 256> parent{}, source{};
    std::array<u8, 1024> directory{};
    for (usize index = 0; index < 256; ++index)
    { parent[index] = clean[parent_at + index]; source[index] = clean[source_at + index]; }
    for (usize index = 0; index < 1024; ++index) directory[index] = clean[t.directory_block * 1024 + index];
    const auto renamed = Ext4DirectoryRenamePlanner::plan(*admitted.info(), 2, parent, 12, source,
        t.directory_block, directory, "notes.txt", "saved-note.txt");
    CHECK(renamed);
    const usize j = t.journal_offset;
    std::fill_n(b.begin() + j + 1024, 6 * 1024, 0);
    std::copy_n(clean.begin() + t.parent_table * 1024, 1024, b.begin() + j + 2048);
    std::copy_n(clean.begin() + t.source_table * 1024, 1024, b.begin() + j + 3072);
    std::copy(renamed.value().after.begin(), renamed.value().after.end(), b.begin() + j + 4096);
    const usize new_parent = j + 2048 + parent_at % 1024;
    const usize new_source = j + 3072 + source_at % 1024;
    w32(b, new_parent + 12, 1700000111); w32(b, new_parent + 16, 1700000111);
    w32(b, new_parent + 132, 123456789U << 2); w32(b, new_parent + 136, 123456789U << 2);
    w32(b, new_parent + 36, r32(b, new_parent + 36) + 1); inode_crc(b, 2, new_parent);
    w32(b, new_source + 12, 1700000111); w32(b, new_source + 132, 123456789U << 2);
    w32(b, new_source + 36, r32(b, new_source + 36) + 1); inode_crc(b, 12, new_source);
    big32(b, j + 1024, jbd2::magic); big32(b, j + 1028, 1); big32(b, j + 1032, t.sequence);
    tag(b, j + 1024 + 12, t.parent_table, 0, j + 2048, t.sequence);
    tag(b, j + 1024 + 44, t.source_table, jbd2::flag_same_uuid, j + 3072, t.sequence);
    tag(b, j + 1024 + 60, t.directory_block, jbd2::flag_same_uuid | jbd2::flag_last_tag,
        j + 4096, t.sequence);
    journal_record_crc(b, j + 1024, 1020);
    big32(b, j + 5120, jbd2::magic); big32(b, j + 5124, 2); big32(b, j + 5128, t.sequence);
    big32(b, j + 5120 + 52, 1700000111); big32(b, j + 5120 + 56, 123456789);
    journal_record_crc(b, j + 5120, 16);
    w32(b, 1024 + 96, r32(b, 1024 + 96) | 4); super_crc(b);
    big32(b, j + 24, t.sequence); big32(b, j + 28, 1);
    big32(b, j + 252, 0); big32(b, j + 252, crc(~u32{0}, b, j, 1024));
    t.completed = b;
    for (const auto &[target, payload] : std::array<std::pair<u32, usize>, 3>{
             {{t.parent_table, j + 2048}, {t.source_table, j + 3072}, {t.directory_block, j + 4096}}})
        std::copy_n(b.begin() + payload, 1024, t.completed.begin() + target * 1024);
    big32(t.completed, j + 24, t.sequence + 2); big32(t.completed, j + 28, 0);
    big32(t.completed, j + 252, 0);
    big32(t.completed, j + 252, crc(~u32{0}, t.completed, j, 1024));
    return t;
}

using Completion = Ext4RecoveryCompletion<4, 2>;
Status run(Disk &disk, Ext4CompletionReport &report)
{
    Completion completion(false, true, Ext4ReplayPolicy::directory_rename);
    const auto result = completion.run(disk, {0, disk.live.size() / 512});
    report = completion.report();
    return result;
}

void reject(const Bytes &image)
{
    Disk disk(image); Ext4CompletionReport report{};
    CHECK(!run(disk, report));
    CHECK(disk.writes == 0 && disk.flushes == 0);
    CHECK(disk.live == image && disk.durable == image);
}

usize entry_offset(const Bytes &image, usize block, std::string_view wanted)
{
    for (usize offset = 0; offset < 1012; offset += r16(image, block + offset + 4))
        if (r32(image, block + offset) && std::string_view{
                reinterpret_cast<const char *>(image.data() + block + offset + 8),
                image[block + offset + 6]} == wanted) return offset;
    CHECK(false); return 1012;
}

void repair_payload(Bytes &image, const Transaction &t)
{
    directory_crc(image, 2, t.journal_offset + 4096);
    tag(image, t.journal_offset + 1024 + 60, t.directory_block,
        jbd2::flag_same_uuid | jbd2::flag_last_tag, t.journal_offset + 4096, t.sequence);
    journal_record_crc(image, t.journal_offset + 1024, 1020);
}

void relocation_tests(const Transaction &t, bool free_record)
{
    const usize home = t.directory_block * 1024, payload = t.journal_offset + 4096;
    const usize old = entry_offset(t.published, home, "notes.txt");
    const usize moved = entry_offset(t.published, payload, "saved-note.txt");
    CHECK(old != moved && t.published[home + old + 6] == 9 && r16(t.published, home + old + 4) == 20);
    CHECK(!r32(t.published, payload + old) && r16(t.published, payload + old + 4) == 20);
    CHECK(r32(t.published, payload + moved) == 12 && t.published[payload + moved + 7] == 1);
    Bytes changed = t.published;
    // Each forgery has valid directory, payload-tag and descriptor checksums.
    changed[payload + old + 8] = 0x5a;
    repair_payload(changed, t); reject(changed);
    changed = t.published;
    changed[payload + moved + 8 + 14] = 0x5a;
    repair_payload(changed, t); reject(changed);
    changed = t.published;
    const usize other = entry_offset(changed, payload, "other-owner");
    changed[payload + other + 8] = 'O';
    repair_payload(changed, t); reject(changed);
    changed = t.published;
    w16(changed, payload + moved + 4, 0);
    repair_payload(changed, t); reject(changed);
    changed = t.published;
    w16(changed, home + old + 4, 0);
    directory_crc(changed, 2, home); reject(changed);
    changed = t.published;
    const usize prior_record = r16(changed, payload + moved + 4);
    CHECK(prior_record >= 32);
    // An alternative valid free-record split changes no live namespace pair.
    w16(changed, payload + moved + 4, 24);
    std::fill(changed.begin() + payload + moved + 24,
              changed.begin() + payload + moved + prior_record, 0);
    w16(changed, payload + moved + 24 + 4, u16(prior_record - 24));
    repair_payload(changed, t); reject(changed);
    changed = t.published;
    // A second source alias must be rejected even with every checksum repaired.
    w32(changed, payload + other, 12);
    repair_payload(changed, t); reject(changed);
    changed = t.published;
    CHECK(r16(changed, payload + other + 4) == 20);
    // Duplicate an unrelated live tuple in the cleared source record. Set-based
    // namespace matching alone would overlook this multiplicity change.
    std::copy_n(changed.begin() + payload + other, 20, changed.begin() + payload + old);
    repair_payload(changed, t); reject(changed);
    changed = t.published;
    // Move the unrelated tuple while preserving its identity and live count.
    std::copy_n(changed.begin() + payload + other, 20, changed.begin() + payload + old);
    std::fill_n(changed.begin() + payload + other, 20, 0);
    w16(changed, payload + other + 4, 20);
    repair_payload(changed, t); reject(changed);
    if (!free_record)
    {
        changed = t.published;
        const usize donor = entry_offset(changed, payload, "init");
        CHECK(donor + r16(changed, payload + donor + 4) == moved);
        changed[payload + donor + 6] = 3;
        repair_payload(changed, t); reject(changed);
    }
}

void tests(const Transaction &t)
{
    Ext4CompletionReport report{};
    {
        Disk disk(t.published);
        CHECK(run(disk, report));
        CHECK(report.complete() && report.validated_actions == 3);
        CHECK(disk.live == t.completed && disk.durable == t.completed);
        CHECK(disk.writes == 4 && disk.flushes == 4);
        Disk repeated(t.completed); CHECK(run(repeated, report));
        CHECK(report.complete() && report.empty_log && repeated.live == t.completed);
    }
    // The table-first action order makes each successful prefix restartable.
    for (usize prefix = 1; prefix <= 3; ++prefix)
    {
        Bytes image = t.published;
        const std::array<u32, 3> targets{t.parent_table, t.source_table, t.directory_block};
        for (usize index = 0; index < prefix; ++index)
            std::copy_n(t.completed.begin() + targets[index] * 1024, 1024,
                        image.begin() + targets[index] * 1024);
        Disk disk(image); CHECK(run(disk, report));
        CHECK(disk.live == t.completed && disk.durable == t.completed);
    }
    // The default non-directory replay profile refuses a directory tag.
    { Disk disk(t.published); Ext4RecoveryCompletion<4, 2> legacy(true, true);
      CHECK(!legacy.run(disk, {0, disk.live.size() / 512}));
      CHECK(!disk.writes && !disk.flushes); }
    auto changed = t.published;
    big32(changed, t.journal_offset + 1024 + 60, u32(t.journal_offset / 1024));
    journal_record_crc(changed, t.journal_offset + 1024, 1020); reject(changed);
    changed = t.published;
    big32(changed, t.journal_offset + 1024 + 60, 1);
    journal_record_crc(changed, t.journal_offset + 1024, 1020); reject(changed);
    changed = t.published;
    big32(changed, t.journal_offset + 1024 + 60,
          r32(changed, inode_offset(changed, 12) + 60));
    journal_record_crc(changed, t.journal_offset + 1024, 1020); reject(changed);
    changed = t.published;
    changed[t.journal_offset + 4096 + 1020] ^= 1;
    tag(changed, t.journal_offset + 1024 + 60, t.directory_block,
        jbd2::flag_same_uuid | jbd2::flag_last_tag, t.journal_offset + 4096, t.sequence);
    journal_record_crc(changed, t.journal_offset + 1024, 1020); reject(changed);
    changed = t.published;
    changed[t.journal_offset + 4096 + 32] ^= 1;
    tag(changed, t.journal_offset + 1024 + 60, t.directory_block,
        jbd2::flag_same_uuid | jbd2::flag_last_tag, t.journal_offset + 4096, t.sequence);
    journal_record_crc(changed, t.journal_offset + 1024, 1020); reject(changed);
    changed = t.published;
    const usize source = t.journal_offset + 3072 + inode_offset(changed, 12) % 1024;
    w32(changed, source + 60, r32(changed, source + 60) + 1);
    inode_crc(changed, 12, source);
    tag(changed, t.journal_offset + 1024 + 44, t.source_table,
        jbd2::flag_same_uuid, t.journal_offset + 3072, t.sequence);
    journal_record_crc(changed, t.journal_offset + 1024, 1020); reject(changed);
    changed = t.published;
    const usize parent = t.journal_offset + 2048 + inode_offset(changed, 2) % 1024;
    w32(changed, parent + 16, 0x80000000U); inode_crc(changed, 2, parent);
    tag(changed, t.journal_offset + 1024 + 12, t.parent_table, 0,
        t.journal_offset + 2048, t.sequence);
    journal_record_crc(changed, t.journal_offset + 1024, 1020); reject(changed);
    changed = t.published;
    usize alias_offset = 1012;
    for (usize offset = 0; offset < 1012; offset += r16(changed, t.directory_block * 1024 + offset + 4))
    {
        const usize at = t.directory_block * 1024 + offset;
        const std::string_view name{reinterpret_cast<const char *>(changed.data() + at + 8),
                                    changed[at + 6]};
        if (name == "other-owner") alias_offset = offset;
    }
    CHECK(alias_offset < 1012);
    w32(changed, t.directory_block * 1024 + alias_offset, 12);
    w32(changed, t.journal_offset + 4096 + alias_offset, 12);
    directory_crc(changed, 2, t.directory_block * 1024);
    directory_crc(changed, 2, t.journal_offset + 4096);
    tag(changed, t.journal_offset + 1024 + 60, t.directory_block,
        jbd2::flag_same_uuid | jbd2::flag_last_tag, t.journal_offset + 4096, t.sequence);
    journal_record_crc(changed, t.journal_offset + 1024, 1020); reject(changed);
    changed = t.published;
    std::copy_n(t.completed.begin() + t.directory_block * 1024, 1024,
                changed.begin() + t.directory_block * 1024);
    reject(changed);
    changed = t.published;
    changed[t.directory_block * 1024 + 1020] ^= 1; reject(changed);
    { Disk disk(t.published); disk.fail_read = 1; CHECK(!run(disk, report));
      CHECK(!disk.writes && !disk.flushes); }
    { Disk disk(t.published); disk.fail_flush = 1; CHECK(!run(disk, report));
      CHECK(!disk.writes && disk.durable == t.published); }
    for (usize failure = 1; failure <= 4; ++failure)
    {
        Disk disk(t.published); disk.fail_write = failure; disk.partial_failure = true;
        CHECK(!run(disk, report) && report.destination_may_have_changed());
        CHECK(report.phase == Ext4CompletionPhase::failed);
    }
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes clean((std::istreambuf_iterator<char>(input)), {});
    CHECK(clean.size() == 32 * 1024 * 1024);
    const auto notes = relocation_fixture::with_notes_source(clean);
    const auto transaction = make_transaction(notes);
    tests(transaction);
    relocation_tests(transaction, false);
    const auto free_transaction = make_transaction(relocation_fixture::with_free_record(notes));
    tests(free_transaction);
    relocation_tests(free_transaction, true);
    std::ofstream output(argv[2], std::ios::binary); CHECK(output);
    output.write(reinterpret_cast<const char *>(transaction.completed.data()),
                 static_cast<std::streamsize>(transaction.completed.size()));
    CHECK(output.good());
    std::puts("PASS private relocation replay: free/slack notes.txt to saved-note.txt; checked JBD2 transaction, durable completion, prefixes, malformed inputs and I/O faults");
}
