#include "ext4_namespace_journal_fixture.hpp"
#include "ext4_owned_support.hpp"
#include <ok/fs/ext4/recovery.hpp>
#include <memory>

using namespace namespace_journal_model;
using Owner = Ext4OwnedTransaction<1, 8>;
using Admission = Ext4NamespaceRecoveryAdmission;

namespace
{
usize mixtures{}, rejects{};

Image dirty_empty(Image image)
{
    fixture::w32(image, 1120, fixture::r32(image, 1120) | 4U);
    fixture::super_crc(image);
    return image;
}

Ext4NamespaceUpdate update_for(const Image &image, bool unlink)
{
    owned_fixture::Disk disk(image);
    Ext4RecoveryBinding binding;
    CHECK(binding.inspect(disk, {0, image.size() / 512}));
    Fixture fixture(image, *binding.info());
    return unlink ? fixture.unlink("basic-note") :
                    fixture.replacement("basic-note", "second-note");
}

Image publish(const Image &image, Ext4NamespaceUpdate update, u64 origin)
{
    owned_fixture::Disk disk(image); disk.origin = origin;
    Owner::Registry registry;
    auto key = registry.register_device(disk, 81); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {origin, image.size() / 512}));
    CHECK(owner->prepare_namespace(update));
    CHECK(owner->commit());
    CHECK(disk.live == disk.durable);
    return disk.durable;
}

void reject(const Image &image, u64 origin)
{
    owned_fixture::Disk disk(image); disk.origin = origin;
    auto admission = std::make_unique<Admission>();
    CHECK(!admission->inspect(disk, {origin, image.size() / 512}));
    CHECK(!admission->ready() && !admission->info() && !admission->result());
    CHECK(!disk.writes && !disk.flushes && disk.live == image && disk.durable == image);
    ++rejects;
}

void exercise(const Image &published, u64 origin, bool expect_unlink)
{
    owned_fixture::Disk disk(published); disk.origin = origin;
    auto baseline = std::make_unique<Admission>();
    CHECK(baseline->inspect(disk, {origin, published.size() / 512}));
    CHECK(baseline->ready() && baseline->report().ready);
    CHECK(baseline->old_binding() && baseline->old_binding()->empty_log &&
          baseline->old_binding()->needs_recovery);
    CHECK(baseline->info() && !baseline->info()->empty_log &&
          baseline->info()->journal_sequence == baseline->journal_sequence());
    CHECK(baseline->report().durable_witness && baseline->result()->durable_old_witness);
    CHECK(baseline->report().old_homes == baseline->count() && !baseline->report().new_homes);
    CHECK(baseline->result()->operation == (expect_unlink ? Ext4NamespaceOperation::unlink :
                                          Ext4NamespaceOperation::replace));
    CHECK(baseline->parsed_next_sequence() == baseline->journal_sequence() + 1);
    CHECK(baseline->linux_cleanup_sequence() == baseline->journal_sequence() + 2);
    CHECK(baseline->scratch()->old_head == baseline->old_head());
    for (usize index = 0; index < baseline->count(); ++index)
    {
        std::array<std::byte, 512> half{};
        const u64 relative = u64{baseline->target(index)} * 2;
        CHECK(baseline->read_old_sectors(relative, half));
        for (usize byte = 0; byte < 512; ++byte)
            CHECK(half[byte] == std::byte((*baseline->before(index))[byte]));
        CHECK(baseline->read_old_sectors(relative + 1, half));
        for (usize byte = 0; byte < 512; ++byte)
            CHECK(half[byte] == std::byte((*baseline->before(index))[512 + byte]));
    }
    std::array<std::byte, 512> old_head_half{};
    CHECK(baseline->read_old_sectors(u64{baseline->journal_first()} * 2 + 1, old_head_half));
    for (usize byte = 0; byte < 512; ++byte)
        CHECK(old_head_half[byte] == std::byte(baseline->old_head()[512 + byte]));
    CHECK(!baseline->read_old_sectors(published.size() / 512, old_head_half));
    CHECK(!disk.writes && !disk.flushes && disk.live == published);
    for (u32 mask = 0; mask < (u32{1} << baseline->count()); ++mask)
    {
        Image mixed = published;
        usize expected_new{};
        for (usize index = 0; index < baseline->count(); ++index)
            if (mask & (u32{1} << index))
            {
                const u32 block = baseline->target(index);
                const auto *after = baseline->after(index); CHECK(after);
                for (usize byte = 0; byte < 1024; ++byte)
                    mixed[block * 1024 + byte] = (*after)[byte];
                ++expected_new;
            }
        owned_fixture::Disk mixed_disk(mixed); mixed_disk.origin = origin;
        auto admission = std::make_unique<Admission>();
        CHECK(admission->inspect(mixed_disk, {origin, mixed.size() / 512}));
        CHECK(admission->count() == baseline->count());
        CHECK(admission->report().new_homes == expected_new);
        CHECK(admission->report().old_homes == baseline->count() - expected_new);
        CHECK(admission->result()->homes.all_before == (expected_new == 0));
        CHECK(admission->result()->homes.all_after == (expected_new == baseline->count()));
        for (usize index = 0; index < admission->count(); ++index)
        {
            CHECK(admission->target(index) == baseline->target(index));
            CHECK(*admission->before(index) == *baseline->before(index));
            CHECK(*admission->after(index) == *baseline->after(index));
            CHECK(*admission->current(index) == ((mask & (u32{1} << index)) ?
                    *baseline->after(index) : *baseline->before(index)));
        }
        CHECK(!mixed_disk.writes && !mixed_disk.flushes && mixed_disk.live == mixed);
        ++mixtures;
    }
    auto bad = published;
    bad[baseline->journal_first() * 1024 + 24] ^= 1;
    reject(bad, origin);
    bad = published;
    bad[(baseline->journal_first() + 1) * 1024 + 128] ^= 1;
    reject(bad, origin);
    bad = published;
    const usize journal_head_at = baseline->journal_first() * 1024;
    const u32 next = baseline->journal_sequence() + 1;
    for (usize byte = 0; byte < 4; ++byte) bad[journal_head_at + 24 + byte] = u8(next >> (24 - byte * 8));
    for (usize byte = 0; byte < 4; ++byte) bad[journal_head_at + 252 + byte] = 0;
    const u32 head_crc = jbd2::detail::crc32c(~u32{0},
        std::span<const u8>{bad}.subspan(journal_head_at, 1024));
    for (usize byte = 0; byte < 4; ++byte) bad[journal_head_at + 252 + byte] = u8(head_crc >> (24 - byte * 8));
    reject(bad, origin);
    bad = published;
    bad[(baseline->journal_first() + 2) * 1024 + 128] ^= 1;
    reject(bad, origin);
    bad = published;
    bad[baseline->target(0) * 1024 + 400] ^= 1;
    reject(bad, origin);
    bad = published;
    bad[1128] ^= 1; fixture::super_crc(bad);
    reject(bad, origin);
    bad = published;
    const usize journal_inode = fixture::inode_offset(bad, 8);
    fixture::w32(bad, journal_inode + 60, fixture::r32(bad, journal_inode + 60) + 1);
    fixture::inode_crc(bad, 8, journal_inode);
    reject(bad, origin);

    owned_fixture::Disk short_disk(published); short_disk.origin = origin;
    auto short_admission = std::make_unique<Admission>();
    CHECK(!short_admission->inspect(short_disk, {origin, 65535}));
    CHECK(!short_disk.writes && !short_disk.flushes);
    ++rejects;
    owned_fixture::Disk failed_disk(published); failed_disk.origin = origin; failed_disk.fail_read = 1;
    auto failed_admission = std::make_unique<Admission>();
    CHECK(!failed_admission->inspect(failed_disk, {origin, published.size() / 512}));
    CHECK(!failed_disk.writes && !failed_disk.flushes);
    ++rejects;
    CHECK(!baseline->inspect(disk, {origin, published.size() / 512}));
    CHECK(baseline->ready());
}

void fresh_session(const Image &published, u64 origin)
{
    owned_fixture::Disk disk(published); disk.origin = origin;
    block::BlockSessions<1> registry;
    auto key = registry.register_device(disk, 91); CHECK(key);
    block::BlockSessions<1>::Session session;
    CHECK(registry.acquire(key.value(), {origin, published.size() / 512},
        block::SessionAccess::writable, session));
    {
        auto admission = std::make_unique<Admission>();
        CHECK(admission->inspect(session, {0, published.size() / 512}));
        CHECK(admission->ready() && admission->info()->layout.first_sector == 0);
        const auto report = session.report(); CHECK(report);
        CHECK(report.value().state == block::DeviceState::owned && report.value().reads > 0 &&
              !report.value().writes && !report.value().flushes);
        std::array<std::byte, 512> old{};
        CHECK(admission->read_old_sectors(u64{admission->target(0)} * 2 + 1, old));
        for (usize byte = 0; byte < 512; ++byte)
            CHECK(old[byte] == std::byte((*admission->before(0))[512 + byte]));
    }
    CHECK(session.close());
    CHECK(!disk.writes && !disk.flushes && disk.live == published && disk.durable == published);
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Image original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    const Image dirty = dirty_empty(original);
    const auto replace = update_for(dirty, false);
    const auto unlink = update_for(dirty, true);
    const auto replaced = publish(dirty, replace, 0);
    const auto unlinked = publish(dirty, unlink, 37);
    exercise(replaced, 0, false);
    exercise(unlinked, 37, true);
    fresh_session(replaced, 0);
    fresh_session(unlinked, 37);
    std::printf("PASS namespace session cold admission mixtures=%zu rejects=%zu device_writes=0 flushes=0 checkpoint=0\n",
        mixtures, rejects);
}
