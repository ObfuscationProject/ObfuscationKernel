#include "ok/fs/jbd2.hpp"

#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::fs::jbd2;

extern "C" u32 ext2fs_crc32c_le(u32, const void *, usize);

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!static_cast<bool>(condition))                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                         \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

using Block = std::array<u8, 1024>;
const std::array<u8, 16> uuid{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
std::array<Block, 10> golden{};

void put(std::span<u8> block, usize offset, u32 value)
{
    block[offset] = u8(value >> 24);
    block[offset + 1] = u8(value >> 16);
    block[offset + 2] = u8(value >> 8);
    block[offset + 3] = u8(value);
}

void seal(std::span<u8> block, usize offset, bool superblock = false)
{
    put(block, offset, 0);
    const auto seed = superblock ? ~u32{0} : ext2fs_crc32c_le(~u32{0}, uuid.data(), uuid.size());
    put(block, offset, ext2fs_crc32c_le(seed, block.data(), block.size()));
}

void payload_checksum(std::span<u8> descriptor, usize tag_offset, u32 sequence, std::span<const u8> payload)
{
    std::array<u8, 4> bytes{};
    put(bytes, 0, sequence);
    auto crc = ext2fs_crc32c_le(~u32{0}, uuid.data(), uuid.size());
    crc = ext2fs_crc32c_le(crc, bytes.data(), bytes.size());
    put(descriptor, tag_offset + 12, ext2fs_crc32c_le(crc, payload.data(), payload.size()));
    seal(descriptor, descriptor.size() - 4);
}

struct Reader
{
    std::array<Block, 16> blocks{};
    u32 read_count{};
    u32 fail_at{~u32{0}};

    Reader()
    {
        // The golden file is packed in traversal order, crossing the ring end.
        for (usize index = 1; index < golden.size(); ++index)
        {
            blocks[(14 + index - 1) % blocks.size()] = golden[index];
        }
    }

    Status read_block(u32 index, std::span<u8> buffer)
    {
        ++read_count;
        if (index == fail_at || index < 1008 || index >= 1024 || buffer.size() != 1024)
        {
            return Status::io_error("injected read failure or unexpected reader bounds");
        }
        for (usize offset = 0; offset < buffer.size(); ++offset)
        {
            buffer[offset] = blocks[index - 1008][offset];
        }
        return Status::success();
    }

    Block &at(u32 position) { return blocks[position - 1008]; }
};

struct Workspace
{
    Block record{};
    Block payload{};
    std::array<ReplayAction, 64> actions{};
    std::array<Revocation, 64> revocations{};
    const std::array<BlockRange, 1> ranges{{{32, 64}}};
    TargetDomain domain{1024, ranges};
    PlanStorage storage() { return {record, payload, actions, revocations}; }
};

Codec codec(Block superblock = golden[0])
{
    auto parsed = Codec::open(superblock, 1024, uuid);
    CHECK(parsed);
    return parsed.value();
}

void golden_tests()
{
    const auto parsed = codec();
    CHECK(parsed.superblock().sequence == 0xffffffffU);
    CHECK(parsed.superblock().start_block == 1022);
    CHECK(parsed.superblock().features == 17);
    CHECK(parsed.superblock().uuid == uuid);
    usize tags = 0;
    CHECK(parsed.descriptor(golden[1], 0xffffffffU, [&](const Tag &tag) {
        CHECK(tag.target_block == 40 + tags);
        CHECK(tag.restore_magic == (tags == 1));
        CHECK(parsed.payload(golden[2 + tags], 0xffffffffU, tag));
        ++tags;
        return Status::success();
    }));
    CHECK(tags == 2);
    auto commit = parsed.commit(golden[4], 0xffffffffU);
    CHECK(commit && commit.value().seconds == 100 && commit.value().nanoseconds == 123456789);
    Reader reader;
    Workspace workspace;
    const auto before = reader.blocks;
    auto result = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(result);
    const auto &plan = result.value();
    CHECK(plan.committed_transactions == 2 && plan.next_sequence == 1 && plan.scanned_blocks == 9);
    CHECK(plan.revoked_actions == 1 && plan.revoke_records == 1 && !plan.incomplete_tail && !plan.empty_log);
    CHECK(plan.actions.size() == 2);
    CHECK(plan.actions[0].target_block == 41 && plan.actions[0].sequence == 0xffffffffU);
    CHECK(plan.actions[0].journal_block == 1008 && plan.actions[0].restore_magic);
    CHECK(plan.actions[1].target_block == 42 && plan.actions[1].sequence == 0);
    CHECK(plan.actions[1].journal_block == 1012 && !plan.actions[1].restore_magic);
    CHECK(reader.blocks == before);
    Reader repeated;
    auto again = build_replay_plan(parsed, repeated, workspace.domain, workspace.storage());
    CHECK(again && again.value().actions.size() == 2 && again.value().revoked_actions == 1);
}

void superblock_tests()
{
    CHECK(!Codec::open(std::span<const u8>{golden[0]}.first(1023), 1024, uuid));
    CHECK(!Codec::open(golden[0], 1023, uuid));
    auto other_uuid = uuid;
    other_uuid[3] ^= 1;
    CHECK(!Codec::open(golden[0], 1024, other_uuid));
    for (auto offset : {usize{0}, usize{100}, usize{252}, usize{700}})
    {
        auto block = golden[0];
        block[offset] ^= 1;
        CHECK(!Codec::open(block, 1024, uuid));
    }
    struct Invalid { usize offset; u32 value; };
    for (const auto item : std::array<Invalid, 21>{{
             {4, 3}, {12, 512}, {12, 8192}, {16, 1023}, {16, 1025}, {20, 0}, {20, 1024},
             {28, 1007}, {28, 1024}, {32, 5}, {36, 1}, {40, 1}, {40, 25}, {40, 21},
             {40, 49}, {40, 0x80000011U}, {44, 1}, {64, 0}, {64, 2}, {68, 1}, {84, 1}}})
    {
        auto block = golden[0];
        put(block, item.offset, item.value);
        seal(block, 252, true);
        CHECK(!Codec::open(block, 1024, uuid));
    }
    auto block = golden[0];
    block[80] = 1;
    seal(block, 252, true);
    CHECK(!Codec::open(block, 1024, uuid));
    put(block, 16, 0xffffffffU);
    put(block, 20, 1);
    block[80] = 4;
    seal(block, 252, true);
    CHECK(!Codec::open(block, 0xffffffffU, uuid));
    for (const auto blocksize : {2048U, 4096U})
    {
        block = golden[0];
        put(block, 12, blocksize);
        seal(block, 252, true);
        const auto parsed = Codec::open(block, 1024, uuid);
        CHECK(parsed && parsed.value().superblock().block_size == blocksize);
        std::array<u8, 4096> commit{};
        for (usize index = 0; index < 60; ++index)
        {
            commit[index] = golden[4][index];
        }
        auto bytes = std::span<u8>{commit}.first(blocksize);
        seal(bytes, 16);
        CHECK(parsed.value().commit(bytes, 0xffffffffU));
        CHECK(!parsed.value().commit(std::span<u8>{commit}.first(blocksize - 1), 0xffffffffU));
    }
    block = golden[0];
    put(block, 28, 0);
    seal(block, 252, true);
    Reader reader;
    Workspace workspace;
    auto empty = build_replay_plan(codec(block), reader, workspace.domain, workspace.storage());
    CHECK(empty && empty.value().empty_log && empty.value().actions.empty() && reader.read_count == 0);
}

void descriptor_tests()
{
    const auto parsed = codec();
    usize visits = 0;
    const auto visitor = [&](const Tag &) { ++visits; return Status::success(); };
    CHECK(!parsed.descriptor(std::span<const u8>{golden[1]}.first(1023), 0xffffffffU, visitor));
    CHECK(!parsed.descriptor(golden[1], 0, visitor));
    CHECK(parsed.descriptor(golden[1], 0xffffffffU, [&](const Tag &tag) {
        CHECK(!parsed.payload(std::span<const u8>{golden[2]}.first(1023), 0xffffffffU, tag));
        return Status::success();
    }));
    for (const auto flags : {2U, 4U, 16U, 0x100U})
    {
        auto block = golden[1];
        put(block, 16, flags);
        seal(block, 1020);
        CHECK(!parsed.descriptor(block, 0xffffffffU, visitor));
    }
    auto block = golden[1];
    block[28] ^= 1;
    seal(block, 1020);
    CHECK(!parsed.descriptor(block, 0xffffffffU, visitor));
    block = golden[1];
    put(block, 20, 1);
    seal(block, 1020);
    CHECK(!parsed.descriptor(block, 0xffffffffU, visitor));
    block = golden[1];
    put(block, 48, 2); // Remove LAST_TAG; following bytes cannot become implicit tags.
    seal(block, 1020);
    CHECK(!parsed.descriptor(block, 0xffffffffU, visitor));
    // Fill all descriptor space with tags, then require a UUID beyond its tail.
    block = golden[1];
    for (usize offset = 44; offset <= 1004; offset += 16)
    {
        put(block, offset, 40);
        put(block, offset + 4, offset == 1004 ? 0 : 2);
        put(block, offset + 8, 0);
    }
    seal(block, 1020);
    CHECK(!parsed.descriptor(block, 0xffffffffU, visitor));
    CHECK(visits == 0); // Structural failure must not publish a partial descriptor.
    CHECK(!parsed.descriptor(golden[1], 0xffffffffU, [](const Tag &) { return Status::no_memory(); }));
    auto payload = golden[2];
    payload[600] ^= 1;
    CHECK(parsed.descriptor(golden[1], 0xffffffffU, [&](const Tag &tag) {
        CHECK(!parsed.payload(payload, 0xffffffffU, tag));
        return Status::success();
    }));
    block = golden[1];
    put(block, 16, 1); // ESCAPE on an unescaped, correctly checksummed payload.
    seal(block, 1020);
    CHECK(parsed.descriptor(block, 0xffffffffU, [&](const Tag &tag) {
        if (tag.target_block == 40) { CHECK(!parsed.payload(golden[2], 0xffffffffU, tag)); }
        return Status::success();
    }));
    payload = golden[2];
    put(payload, 0, magic);
    block = golden[1];
    payload_checksum(block, 12, 0xffffffffU, payload);
    CHECK(parsed.descriptor(block, 0xffffffffU, [&](const Tag &tag) {
        if (tag.target_block == 40) { CHECK(!parsed.payload(payload, 0xffffffffU, tag)); }
        return Status::success();
    }));
}

void revoke_commit_tests()
{
    const auto parsed = codec();
    usize count = 0;
    CHECK(!parsed.revoke(std::span<const u8>{golden[5]}.first(1023), 0, [](u64) { return Status::success(); }));
    CHECK(!parsed.commit(std::span<const u8>{golden[4]}.first(1023), 0xffffffffU));
    CHECK(parsed.revoke(golden[5], 0, [&](u64 target) {
        CHECK(target == 40);
        ++count;
        return Status::success();
    }));
    CHECK(count == 1);
    for (const auto invalid : {0U, 15U, 17U, 19U, 1021U, 1024U, 0xffffffffU})
    {
        auto block = golden[5];
        put(block, 12, invalid);
        seal(block, 1020);
        CHECK(!parsed.revoke(block, 0, [](u64) { return Status::success(); }));
    }
    auto sb = golden[0];
    put(sb, 40, 16);
    seal(sb, 252, true);
    CHECK(!codec(sb).revoke(golden[5], 0, [](u64) { return Status::success(); }));
    for (const auto offset : {usize{12}, usize{13}, usize{14}, usize{20}, usize{47}})
    {
        auto block = golden[4];
        block[offset] = 1;
        seal(block, 16);
        CHECK(!parsed.commit(block, 0xffffffffU));
    }
    auto block = golden[4];
    put(block, 56, 1000000000);
    seal(block, 16);
    CHECK(!parsed.commit(block, 0xffffffffU));
    block = golden[4];
    block[1023] ^= 1; // Commit checksum includes padding through the whole block.
    CHECK(!parsed.commit(block, 0xffffffffU));
}

void plan_failure_tests()
{
    const auto parsed = codec();
    for (const auto position : {1022U, 1023U, 1008U, 1009U, 1010U, 1011U, 1012U, 1013U, 1014U})
    {
        Reader reader;
        Workspace workspace;
        reader.at(position)[800] ^= 1;
        CHECK(!build_replay_plan(parsed, reader, workspace.domain, workspace.storage()));
    }
    for (const auto position : {1022U, 1023U, 1009U, 1010U, 1014U})
    {
        Reader reader;
        Workspace workspace;
        reader.fail_at = position;
        CHECK(!build_replay_plan(parsed, reader, workspace.domain, workspace.storage()));
    }
    for (const auto target : {0U, 31U, 96U, 1024U, 0xffffffffU})
    {
        Reader reader;
        Workspace workspace;
        put(reader.at(1022), 12, target);
        seal(reader.at(1022), 1020);
        CHECK(!build_replay_plan(parsed, reader, workspace.domain, workspace.storage()));
        reader = Reader{};
        put(reader.at(1010), 16, target);
        seal(reader.at(1010), 1020);
        CHECK(!build_replay_plan(parsed, reader, workspace.domain, workspace.storage()));
    }
    for (const auto sequence : {2U, 0x80000001U})
    {
        Reader reader;
        Workspace workspace;
        put(reader.at(1014), 8, sequence);
        seal(reader.at(1014), 16);
        CHECK(!build_replay_plan(parsed, reader, workspace.domain, workspace.storage()));
    }
    Reader reader;
    Workspace workspace;
    auto storage = workspace.storage();
    storage.actions = storage.actions.first(2);
    auto limited = build_replay_plan(parsed, reader, workspace.domain, storage);
    CHECK(!limited && limited.status().code() == StatusCode::no_memory);
    reader = Reader{};
    storage = workspace.storage();
    storage.revocations = {};
    limited = build_replay_plan(parsed, reader, workspace.domain, storage);
    CHECK(!limited && limited.status().code() == StatusCode::no_memory);
    reader = Reader{};
    storage = workspace.storage();
    storage.payload_block = storage.payload_block.first(1023);
    CHECK(!build_replay_plan(parsed, reader, workspace.domain, storage));
    const std::array<BlockRange, 1> invalid{{{~u64{0} - 1, 4}}};
    CHECK(!build_replay_plan(parsed, reader, TargetDomain{~u64{0}, invalid}, workspace.storage()));
    CHECK(!build_replay_plan(parsed, reader, TargetDomain{}, workspace.storage()));
    reader = Reader{};
    reader.at(1014).fill(0x9a);
    CHECK(!build_replay_plan(parsed, reader, workspace.domain, workspace.storage()));
    reader = Reader{};
    put(reader.at(1014), 4, 99);
    CHECK(!build_replay_plan(parsed, reader, workspace.domain, workspace.storage()));
    // A ring entirely filled with consecutive empty commits must not loop or publish.
    reader = Reader{};
    u32 sequence = 0xffffffffU;
    for (u32 index = 0; index < 16; ++index)
    {
        const auto position = 1008 + (14 + index) % 16;
        reader.at(position) = golden[4];
        put(reader.at(position), 8, sequence++);
        seal(reader.at(position), 16);
    }
    CHECK(!build_replay_plan(parsed, reader, workspace.domain, workspace.storage()));
    CHECK(reader.read_count == 16);
}

void commit_boundary_tests()
{
    const auto parsed = codec();
    Workspace workspace;
    Reader reader;
    reader.at(1013).fill(0); // Uncommitted second transaction and its revoke are excluded.
    auto incomplete = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(incomplete && incomplete.value().committed_transactions == 1);
    CHECK(incomplete.value().actions.size() == 2 && incomplete.value().revoked_actions == 0);
    CHECK(incomplete.value().incomplete_tail && incomplete.value().next_sequence == 0);
    reader = Reader{};
    reader.at(1009).fill(0);
    auto first_incomplete = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(first_incomplete && first_incomplete.value().actions.empty());
    CHECK(first_incomplete.value().committed_transactions == 0 && first_incomplete.value().incomplete_tail);
    reader = Reader{};
    reader.at(1014).fill(0);
    auto zero_end = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(zero_end && zero_end.value().actions.size() == 2 && !zero_end.value().incomplete_tail);
    // Revoke in the same transaction suppresses that transaction's payload too.
    reader = Reader{};
    put(reader.at(1010), 16, 41);
    seal(reader.at(1010), 1020);
    put(reader.at(1011), 12, 41);
    seal(reader.at(1011), 1020);
    auto same = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(same && same.value().actions.size() == 1 && same.value().revoked_actions == 2);
    CHECK(same.value().actions[0].target_block == 40);
    // A revoke from the earlier transaction must permit later reuse.
    reader = Reader{};
    reader.at(1009) = golden[5];
    put(reader.at(1009), 8, 0xffffffffU);
    seal(reader.at(1009), 1020);
    reader.at(1010) = golden[4];
    put(reader.at(1011), 12, 40);
    seal(reader.at(1011), 1020);
    auto reused = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(reused && reused.value().actions.size() == 2 && reused.value().revoked_actions == 1);
    CHECK(reused.value().actions[1].sequence == 0 && reused.value().actions[1].target_block == 40);
}

void descriptor_order_tests()
{
    const auto parsed = codec();
    Reader reader;
    Workspace workspace;
    put(reader.at(1022), 12, 42);
    put(reader.at(1022), 44, 42);
    seal(reader.at(1022), 1020);
    const auto duplicates = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(duplicates && duplicates.value().actions.size() == 3 && duplicates.value().revoked_actions == 0);
    CHECK(duplicates.value().actions[0].journal_block == 1023);
    CHECK(duplicates.value().actions[1].journal_block == 1008);
    CHECK(duplicates.value().actions[2].journal_block == 1012);
    reader = Reader{};
    reader.at(1013) = golden[6];
    put(reader.at(1013), 12, 43);
    seal(reader.at(1013), 1020);
    reader.at(1014) = golden[7];
    reader.at(1015) = golden[8];
    const auto multiple = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(multiple && multiple.value().actions.size() == 3 && multiple.value().revoked_actions == 1);
    CHECK(multiple.value().committed_transactions == 2 && multiple.value().scanned_blocks == 11);
    CHECK(multiple.value().actions[1].target_block == 42 && multiple.value().actions[2].target_block == 43);
    CHECK(multiple.value().actions[1].sequence == multiple.value().actions[2].sequence);
}

void revoke_record_count_tests()
{
    const auto parsed = codec();
    Workspace workspace;
    Reader reader;
    // A valid target that no descriptor writes suppresses no action.
    put(reader.at(1010), 16, 43);
    seal(reader.at(1010), 1020);
    auto nonsuppressing = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(nonsuppressing && nonsuppressing.value().revoke_records == 1 &&
          nonsuppressing.value().revoked_actions == 0 && nonsuppressing.value().actions.size() == 3);
    reader = Reader{};
    put(reader.at(1010), 12, 16);
    for (usize at = 16; at < 1020; ++at) reader.at(1010)[at] = 0;
    seal(reader.at(1010), 1020);
    auto empty = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(empty && empty.value().revoke_records == 1 && empty.value().revoked_actions == 0 &&
          empty.value().actions.size() == 3 && !empty.value().incomplete_tail);
    reader.at(1013).fill(0);
    auto pending = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(pending && pending.value().revoke_records == 1 && pending.value().revoked_actions == 0 &&
          pending.value().incomplete_tail && pending.value().committed_transactions == 1);
    reader = Reader{};
    reader.at(1010) = golden[5];
    put(reader.at(1010), 8, 0xffffffffU); // A validated stale boundary is not active.
    seal(reader.at(1010), 1020);
    auto stale = build_replay_plan(parsed, reader, workspace.domain, workspace.storage());
    CHECK(stale && stale.value().revoke_records == 0 && stale.value().revoked_actions == 0 &&
          stale.value().committed_transactions == 1 && !stale.value().incomplete_tail);
}

void wide_target_tests()
{
    auto sb = golden[0];
    put(sb, 40, 19);
    seal(sb, 252, true);
    const auto parsed = codec(sb);
    Reader reader;
    put(reader.at(1022), 20, 1); // high 32 bits of first target
    seal(reader.at(1022), 1020);
    put(reader.at(1011), 20, 1);
    seal(reader.at(1011), 1020);
    put(reader.at(1010), 12, 24);
    put(reader.at(1010), 16, 1);
    put(reader.at(1010), 20, 40);
    seal(reader.at(1010), 1020);
    Workspace workspace;
    const std::array<BlockRange, 2> ranges{{{32, 64}, {0x100000020ULL, 64}}};
    auto result = build_replay_plan(parsed, reader, TargetDomain{0x100000400ULL, ranges}, workspace.storage());
    CHECK(result && result.value().revoked_actions == 1 && result.value().actions.size() == 2);
    CHECK(result.value().actions[1].target_block == 0x10000002aULL);
    put(reader.at(1010), 12, 20); // 64-bit entries cannot have 32-bit alignment.
    seal(reader.at(1010), 1020);
    CHECK(!build_replay_plan(parsed, reader, TargetDomain{0x100000400ULL, ranges}, workspace.storage()));
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    auto *file = std::fopen(argv[1], "rb");
    CHECK(file != nullptr);
    CHECK(std::fread(golden.data(), sizeof(Block), golden.size(), file) == golden.size());
    CHECK(std::fgetc(file) == EOF);
    CHECK(std::fclose(file) == 0);
    CHECK(ext2fs_crc32c_le(~u32{0}, "123456789", 9) == 0x1cf96d7cU);
    golden_tests();
    superblock_tests();
    descriptor_tests();
    revoke_commit_tests();
    plan_failure_tests();
    commit_boundary_tests();
    descriptor_order_tests();
    revoke_record_count_tests();
    wide_target_tests();
    std::puts("JBD2 codec/replay-plan HOST MODEL passed; no disk replay, journal writes or mount acceptance");
}
