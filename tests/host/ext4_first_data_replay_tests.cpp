#include "ext4_completion_support.hpp"
#include <ok/fs/ext4/txn.hpp>
#include <ok/fs/ext4/extent.hpp>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace fixture;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL first data replay:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace
{
using Block = Ext4FirstDataBlock;

void be32(std::span<u8> bytes, usize at, u32 value)
{
    bytes[at] = u8(value >> 24); bytes[at + 1] = u8(value >> 16);
    bytes[at + 2] = u8(value >> 8); bytes[at + 3] = u8(value);
}
Block get(const Bytes &image, u32 block)
{
    CHECK((u64{block} + 1) * 1024 <= image.size());
    Block result{};
    std::copy_n(image.begin() + usize(block) * 1024, 1024, result.begin());
    return result;
}
void put(Bytes &image, u32 block, const Block &source)
{
    CHECK((u64{block} + 1) * 1024 <= image.size());
    std::copy(source.begin(), source.end(), image.begin() + usize(block) * 1024);
}
u32 checksum(u32 seed_value, std::span<const u8> bytes)
{ return ext2fs_crc32c_le(seed_value, bytes.data(), bytes.size()); }

struct Disk final : driver::BlockDevice
{
    explicit Disk(Bytes source) : image(std::move(source)), durable(image) {}
    struct Operation { char kind; u64 sector; };
    Bytes image, durable;
    std::vector<Operation> operations;
    usize writes{}, flushes{};
    usize fail_write{}, fail_flush{};
    bool fail_after_completion{};
    driver::BlockGeometry geometry() const override { return {image.size() / 512, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector >= image.size() / 512 || out.size() % 512 ||
            out.size() / 512 > image.size() / 512 - sector)
            return Status::io_error("first data replay test read bounds");
        std::copy_n(image.begin() + sector * 512, out.size(), reinterpret_cast<u8 *>(out.data()));
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> bytes) override
    {
        ++writes; operations.push_back({'w', sector});
        if (writes == fail_write && !fail_after_completion)
            return Status::io_error("injected first-data replay write before completion");
        if (sector >= image.size() / 512 || bytes.size() != 1024 ||
            bytes.size() / 512 > image.size() / 512 - sector)
            return Status::io_error("first data replay test write bounds");
        std::copy_n(reinterpret_cast<const u8 *>(bytes.data()), bytes.size(), image.begin() + sector * 512);
        if (writes == fail_write)
            return Status::io_error("injected first-data replay write after completion");
        return {};
    }
    Status flush() override
    {
        ++flushes; operations.push_back({'f', 0});
        if (flushes == fail_flush && !fail_after_completion)
            return Status::io_error("injected first-data replay flush before completion");
        durable = image;
        if (flushes == fail_flush)
            return Status::io_error("injected first-data replay flush after completion");
        return {};
    }
};

struct Published
{
    Bytes image;
    Ext4FirstDataPlan plan;
    u32 journal{};
};

Published publish(const Bytes &source)
{
    Disk disk(source);
    Ext4RecoveryBinding binding;
    CHECK(binding.inspect(disk, {0, disk.image.size() / 512}));
    CHECK(binding.info()->needs_recovery && binding.info()->empty_log);
    const u32 inode = 14;
    const auto super = get(source, 1), descriptors = get(source, 2);
    const u32 group = 0, bitmap_block = r32(source, descriptor(source, group));
    const auto bitmap = get(source, bitmap_block);
    const auto chosen = Ext4FirstDataPlanner::select_block(*binding.info(),
        super, descriptors, bitmap, group);
    CHECK(chosen);
    const u32 physical = chosen.value();
    const u32 table_block = r32(source, descriptor(source, 0) + 8) + (inode - 1) / 4;
    const auto table = get(source, table_block);
    Ext4FirstDataPlan plan{};
    const Ext4FirstDataUpdate update{inode, 19, 1700000111, 123456789};
    const Ext4FirstDataSource inputs{group, physical, table_block, super, descriptors, bitmap, table};
    const auto planned = Ext4FirstDataPlanner::plan(*binding.info(), update, inputs, plan);
    if (!planned)
        std::fprintf(stderr, "first data fixture planner code=%u reason=%.*s\n", u32(planned.code()),
                     int(planned.message().size()), planned.message().data());
    CHECK(planned && plan.count == 4);
    CHECK(plan.targets[0] == bitmap_block && plan.targets[1] == 2 && plan.targets[2] == 1 &&
          plan.targets[3] == table_block);

    Bytes image = source;
    Block data{};
    constexpr char text[] = "first data replay!!";
    for (usize index = 0; index < 19; ++index) data[index] = u8(text[index]);
    put(image, physical, data);

    const u32 journal_block = u32(journal(source) / 1024);
    Block head = get(image, journal_block), descriptor_record{}, commit{}, end{};
    CHECK(jbd2::detail::be32(head, 28) == 0);
    const u32 sequence = jbd2::detail::be32(head, 24) + 1;
    CHECK(sequence > 1);
    be32(descriptor_record, 0, jbd2::magic);
    be32(descriptor_record, 4, 1);
    be32(descriptor_record, 8, sequence);
    be32(commit, 0, jbd2::magic);
    be32(commit, 4, 2);
    be32(commit, 8, sequence);
    const u32 seed_value = seed(image);
    for (usize index = 0; index < 4; ++index)
    {
        Block payload = plan.after[index];
        u32 flags = (index ? jbd2::flag_same_uuid : 0) |
                    (index == 3 ? jbd2::flag_last_tag : 0);
        if (jbd2::detail::be32(payload, 0) == jbd2::magic)
        { be32(payload, 0, 0); flags |= jbd2::flag_escape; }
        const usize tag = index ? 44 + (index - 1) * 16 : 12;
        be32(descriptor_record, tag, plan.targets[index]);
        be32(descriptor_record, tag + 4, flags);
        const u32 tagged = checksum(checksum(seed_value,
            std::span<const u8>{descriptor_record}.subspan(8, 4)), payload);
        be32(descriptor_record, tag + 12, tagged);
        put(image, journal_block + u32(index) + 2, payload);
    }
    be32(descriptor_record, 1020, checksum(seed_value, descriptor_record));
    be32(commit, 52, update.seconds);
    be32(commit, 56, update.nanoseconds);
    be32(commit, 16, checksum(seed_value, commit));
    be32(head, 24, sequence); be32(head, 28, 1);
    be32(head, 252, 0); be32(head, 252, checksum(~u32{0}, head));
    put(image, journal_block + 1, descriptor_record);
    put(image, journal_block + 6, commit);
    put(image, journal_block + 7, end);
    put(image, journal_block, head);
    CHECK(disk.writes == 0 && disk.flushes == 0);
    return {std::move(image), plan, journal_block};
}

void home_after(Bytes &image, const Published &published, usize index)
{ put(image, published.plan.targets[index], published.plan.after[index]); }

void journal_descriptor_checksum(Bytes &image, const Published &published)
{
    Block descriptor_record = get(image, published.journal + 1);
    be32(descriptor_record, 1020, 0);
    be32(descriptor_record, 1020, checksum(seed(image), descriptor_record));
    put(image, published.journal + 1, descriptor_record);
}

void journal_payload_checksum(Bytes &image, const Published &published, usize index)
{
    Block descriptor_record = get(image, published.journal + 1);
    const Block payload = get(image, published.journal + u32(index) + 2);
    const usize tag = index ? 44 + (index - 1) * 16 : 12;
    const u32 tagged = checksum(checksum(seed(image),
        std::span<const u8>{descriptor_record}.subspan(8, 4)), payload);
    be32(descriptor_record, tag + 12, tagged);
    put(image, published.journal + 1, descriptor_record);
    journal_descriptor_checksum(image, published);
}

void accepted(Bytes image, const Published &published)
{
    Disk disk(std::move(image));
    auto completion = std::make_unique<Ext4RecoveryCompletion<4, 2>>(
        false, true, Ext4ReplayPolicy::first_data_allocation);
    bool all_after = true;
    for (usize index = 0; index < 4; ++index)
        all_after = all_after && get(disk.image, published.plan.targets[index]) == published.plan.after[index];
    const auto result = completion->run(disk, {0, disk.image.size() / 512});
    if (!result)
        std::fprintf(stderr, "first data replay rejected code=%u reason=%.*s\n", u32(result.code()),
                     int(result.message().size()), result.message().data());
    CHECK(result && completion->report().complete());
    CHECK(completion->report().validated_actions == 4);
    CHECK(disk.writes == (all_after ? 1 : 5) && disk.flushes == (all_after ? 2 : 4));
    usize cursor{};
    const auto expect_flush = [&]()
    { CHECK(cursor < disk.operations.size() && disk.operations[cursor++].kind == 'f'); };
    const auto expect_write = [&](u32 block)
    {
        CHECK(cursor < disk.operations.size() && disk.operations[cursor].kind == 'w' &&
              disk.operations[cursor].sector == u64{block} * 2);
        ++cursor;
    };
    expect_flush();
    if (!all_after)
    {
        for (usize index = 0; index < 3; ++index) expect_write(published.plan.targets[index]);
        expect_flush(); expect_write(published.plan.targets[3]); expect_flush();
    }
    expect_write(published.journal); expect_flush();
    CHECK(cursor == disk.operations.size());
    CHECK(jbd2::detail::be32(get(disk.image, published.journal), 28) == 0);
    for (usize index = 0; index < 4; ++index)
        CHECK(get(disk.image, published.plan.targets[index]) == published.plan.after[index]);
}

void rejected(Bytes image)
{
    Disk disk(std::move(image));
    auto completion = std::make_unique<Ext4RecoveryCompletion<4, 2>>(
        false, true, Ext4ReplayPolicy::first_data_allocation);
    CHECK(!completion->run(disk, {0, disk.image.size() / 512}));
    CHECK(disk.writes == 0 && disk.flushes == 0);
}

void restarted(Bytes image, const Published &published, usize write_failure,
               usize flush_failure, bool after_completion)
{
    Disk disk(std::move(image));
    disk.fail_write = write_failure;
    disk.fail_flush = flush_failure;
    disk.fail_after_completion = after_completion;
    auto completion = std::make_unique<Ext4RecoveryCompletion<4, 2>>(
        false, true, Ext4ReplayPolicy::first_data_allocation);
    CHECK(!completion->run(disk, {0, disk.image.size() / 512}));
    CHECK(completion->report().destination_may_have_changed() == (disk.writes != 0));
    Bytes reboot = std::move(disk.durable);
    if (jbd2::detail::be32(get(reboot, published.journal), 28) != 0)
    {
        accepted(std::move(reboot), published);
        return;
    }
    Disk resumed(std::move(reboot));
    auto retry = std::make_unique<Ext4RecoveryCompletion<4, 2>>(
        false, true, Ext4ReplayPolicy::first_data_allocation);
    CHECK(retry->run(resumed, {0, resumed.image.size() / 512}));
    CHECK(retry->report().complete() && retry->report().empty_log);
    CHECK(resumed.writes == 0 && resumed.flushes == 1);
    for (usize index = 0; index < 4; ++index)
        CHECK(get(resumed.image, published.plan.targets[index]) == published.plan.after[index]);
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes source((std::istreambuf_iterator<char>(input)), {});
    CHECK(source.size() == 32 * 1024 * 1024);
    const auto published = publish(source);
    for (usize mask = 0; mask < 8; ++mask)
    {
        Bytes image = published.image;
        for (usize index = 0; index < 3; ++index)
            if (mask & (usize{1} << index)) home_after(image, published, index);
        accepted(std::move(image), published);
    }
    {
        Bytes image = published.image;
        for (usize index = 0; index < 4; ++index) home_after(image, published, index);
        accepted(std::move(image), published);
    }
    for (usize mask = 0; mask < 7; ++mask)
    {
        Bytes image = published.image;
        for (usize index = 0; index < 3; ++index)
            if (mask & (usize{1} << index)) home_after(image, published, index);
        home_after(image, published, 3);
        rejected(std::move(image));
    }
    {
        Bytes image = published.image;
        image[usize(published.plan.targets[0]) * 1024 + 17] ^= 0x80;
        rejected(std::move(image));
    }
    {
        Bytes image = published.image;
        image[usize(published.plan.targets[1]) * 1024 + 14] ^= 1;
        rejected(std::move(image));
    }
    {
        Bytes image = published.image;
        image[(usize(published.journal) + 3) * 1024 + 50] ^= 1;
        rejected(std::move(image));
    }
    {
        // A syntactically valid journal record cannot redirect the allocated
        // bitmap action to another home even with a corrected descriptor CRC.
        Bytes image = published.image;
        Block descriptor_record = get(image, published.journal + 1);
        be32(descriptor_record, 12, published.plan.targets[0] + 1);
        put(image, published.journal + 1, descriptor_record);
        journal_descriptor_checksum(image, published);
        rejected(std::move(image));
    }
    {
        // The JBD2 tag and descriptor checksums both match this altered
        // payload; only the ext4/canonical allocation proof may reject it.
        Bytes image = published.image;
        Block payload = get(image, published.journal + 2);
        payload[1023] ^= 1;
        put(image, published.journal + 2, payload);
        journal_payload_checksum(image, published, 0);
        rejected(std::move(image));
    }
    {
        Bytes image = published.image;
        const usize at = inode_offset(image, 12);
        w32(image, at + 60, published.plan.physical);
        inode_crc(image, 12, at);
        rejected(std::move(image));
    }
    for (usize ordinal = 1; ordinal <= 5; ++ordinal)
        for (bool after_completion : {false, true})
            restarted(published.image, published, ordinal, 0, after_completion);
    for (usize ordinal = 1; ordinal <= 4; ++ordinal)
        for (bool after_completion : {false, true})
            restarted(published.image, published, 0, ordinal, after_completion);
    {
        Bytes image = published.image;
        for (usize index = 0; index < 4; ++index) home_after(image, published, index);
        for (bool after_completion : {false, true})
        {
            restarted(image, published, 1, 0, after_completion);
            for (usize ordinal = 1; ordinal <= 2; ++ordinal)
                restarted(image, published, 0, ordinal, after_completion);
        }
    }
    std::puts("PASS private first-data replay: 8 old-inode combinations, all-after cleanup, 24 restart cuts, barriers and zero-I/O rejection");
}
