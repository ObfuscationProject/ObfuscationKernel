#include "ext4_namespace_journal_fixture.hpp"
#include "ext4_owned_support.hpp"
#include <memory>

using Owner = ok::fs::Ext4OwnedTransaction<1, 8>;
using namespace namespace_journal_model;

namespace
{
Image dirty_empty(Image image)
{
    fixture::w32(image, 1120, fixture::r32(image, 1120) | 4U);
    fixture::super_crc(image);
    return image;
}

Ext4NamespaceUpdate operation(const Image &image)
{
    owned_fixture::Disk disk(image);
    Ext4RecoveryBinding binding;
    CHECK(binding.inspect(disk, {0, image.size() / 512}));
    Fixture fixture(image, *binding.info());
    return fixture.replacement("basic-note", "second-note");
}

struct Published
{
    Image bytes;
    usize homes{}, journal_first{};
};

Published successful(const Image &image, Ext4NamespaceUpdate update)
{
    owned_fixture::Disk disk(image);
    Owner::Registry registry;
    const auto key = registry.register_device(disk, 37); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, image.size() / 512}));
    CHECK(owner->prepare_namespace(update));
    CHECK(owner->report().phase == Ext4OwnedPhase::prepared);
    CHECK(!disk.writes && !disk.flushes);
    const usize homes = owner->buffers().read_pins - 1;
    CHECK(homes >= 2 && homes <= 5);
    const usize first = fixture::journal(image) / 1024;
    const usize start_trace = disk.trace.size();
    CHECK(owner->commit());
    const auto published = owner->report();
    CHECK(published.phase == Ext4OwnedPhase::durable && published.writer.published());
    CHECK(published.writer.write_attempts == homes + 4);
    CHECK(published.writer.flush_attempts == 4);
    CHECK(disk.live == disk.durable);
    std::vector<owned_fixture::Trace> operations;
    for (usize index = start_trace; index < disk.trace.size(); ++index)
        if (disk.trace[index].operation != 'r') operations.push_back(disk.trace[index]);
    CHECK(operations.size() == homes + 8);
    CHECK(operations[0].operation == 'f');
    CHECK(operations[1].operation == 'w' && operations[1].sector == (first + 1) * 2);
    CHECK(operations[homes + 2].operation == 'w' && operations[homes + 2].sector == (first + homes + 3) * 2);
    CHECK(operations[homes + 3].operation == 'f');
    CHECK(operations[homes + 4].operation == 'w' && operations[homes + 4].sector == (first + homes + 2) * 2);
    CHECK(operations[homes + 5].operation == 'f');
    CHECK(operations[homes + 6].operation == 'w' && operations[homes + 6].sector == first * 2);
    CHECK(operations[homes + 7].operation == 'f');
    CHECK(jbd2::detail::be32(load(disk.durable, u32(first)), 28) == 1);
    CHECK(Ext4NamespaceWitnessCodec::present(load(disk.durable, u32(first + 1))));
    Published result{disk.durable, homes, first};
    const usize writes = disk.writes, flushes = disk.flushes;
    CHECK(owner->checkpoint());
    CHECK(owner->report().phase == Ext4OwnedPhase::orphan_cleanup_pending &&
          owner->report().completion.complete() && owner->report().orphan_cleanup_pending);
    CHECK(disk.writes == writes + homes + 1 && disk.flushes == flushes + 3 && disk.live == disk.durable);
    CHECK(!owner->close());
    return result;
}

void bad_preparation(const Image &image, Ext4NamespaceUpdate update)
{
    owned_fixture::Disk disk(image);
    Owner::Registry registry;
    const auto key = registry.register_device(disk, 38); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, image.size() / 512}));
    CHECK(!owner->prepare_namespace(update));
    CHECK(owner->report().phase == Ext4OwnedPhase::rejected ||
          owner->report().phase == Ext4OwnedPhase::failed);
    CHECK(!disk.writes && !disk.flushes && disk.live == image && disk.durable == image);
}

void publication_fault(const Image &image, Ext4NamespaceUpdate update, usize write, usize flush,
                       bool after = false, usize cut = 0)
{
    owned_fixture::Disk disk(image);
    Owner::Registry registry;
    const auto key = registry.register_device(disk, 39); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, image.size() / 512}));
    CHECK(owner->prepare_namespace(update));
    disk.fail_write = after ? 0 : write;
    disk.fail_flush = after ? 0 : flush;
    disk.fail_write_after = after ? write : 0;
    disk.fail_flush_after = after ? flush : 0;
    disk.cut = cut;
    CHECK(!owner->commit());
    CHECK(owner->report().phase == Ext4OwnedPhase::failed);
    CHECK(owner->buffers().state == MetadataOwnerState::failed);
    CHECK(!owner->checkpoint());
}

}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Image original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    const auto dirty = dirty_empty(original);
    const auto update = operation(dirty);
    auto bad = update; bad.nanoseconds = 1000000000U; bad_preparation(dirty, bad);
    bad = update; ++bad.source_generation; bad_preparation(dirty, bad);
    const auto published = successful(dirty, update);
    CHECK(published.homes >= 2 && published.homes <= 5);
    for (usize index = 1; index <= published.homes + 4; ++index)
    {
        publication_fault(dirty, update, index, 0);
        publication_fault(dirty, update, index, 0, true);
    }
    for (usize index = 1; index <= 4; ++index)
    {
        publication_fault(dirty, update, 0, index);
        publication_fault(dirty, update, 0, index, true);
    }
    for (usize bytes : {1U, 28U, 64U, 256U, 512U, 1023U})
        publication_fault(dirty, update, published.homes + 4, 0, false, bytes);
    std::puts("PASS private namespace writer: ordered publication, checked checkpoint, orphan pending, rejected input and failure quarantine");
}
