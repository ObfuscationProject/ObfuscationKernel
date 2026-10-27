#include "ext4_rename_relocation_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL owned rename faults:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace
{
using Owner = Ext4OwnedTransaction<1, 8>;
constexpr Ext4RenameUpdate rename_update{2, 12, 1700000111, 123456789, "notes.txt", "saved-note.txt"};

Bytes dirty_empty(Bytes image)
{
    w32(image, 1120, r32(image, 1120) | 4U);
    super_crc(image);
    return image;
}

void restart(const Bytes &durable, bool renamed)
{
    Disk disk(durable);
    Ext4MountedMetadata<1>::Registry registry;
    auto key = registry.register_device(disk, 19); CHECK(key);
    auto mount = std::make_unique<Ext4MountedMetadata<1>>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(mount->mount(registry, key.value(), {0, durable.size() / 512}));
    auto *root = mount->root(); CHECK(root);
    const auto present = root->lookup_child(renamed ? "saved-note.txt" : "notes.txt");
    CHECK(present);
    CHECK(present.value()->query_metadata().value().attributes.inode == 12);
    CHECK(root->lookup_child(renamed ? "notes.txt" : "saved-note.txt").status().code() == StatusCode::not_found);
    CHECK(disk.live == disk.durable);
}

void rejected(const Bytes &dirty, Ext4RenameUpdate update, StatusCode expected)
{
    Disk disk(dirty); Owner::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    const auto status = owner->prepare_rename(update);
    CHECK(status.code() == expected);
    CHECK(owner->report().phase == Ext4OwnedPhase::rejected);
    CHECK(owner->buffers().read_pins == 0 && !owner->buffers().transaction_active);
    CHECK(!disk.writes && !disk.flushes && disk.live == dirty && disk.durable == dirty);
    CHECK(owner->close());
}

void publication_fault(const Bytes &dirty, usize write, usize flush)
{
    Disk disk(dirty); Owner::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    CHECK(owner->prepare_rename(rename_update));
    disk.fail_write = write; disk.fail_flush = flush;
    CHECK(!owner->commit());
    CHECK(owner->report().phase == Ext4OwnedPhase::failed);
    CHECK(owner->buffers().state == MetadataOwnerState::failed);
    disk.crash();
    restart(disk.durable, false);
}

void checkpoint_fault(const Bytes &dirty, usize write, usize flush)
{
    Disk disk(dirty); Owner::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    CHECK(owner->prepare_rename(rename_update));
    CHECK(owner->commit());
    const usize writes = disk.writes, flushes = disk.flushes;
    disk.fail_write = write ? writes + write : 0;
    disk.fail_flush = flush ? flushes + flush : 0;
    CHECK(!owner->checkpoint());
    CHECK(owner->report().phase == Ext4OwnedPhase::failed);
    CHECK(owner->buffers().state == MetadataOwnerState::failed);
    disk.crash();
    restart(disk.durable, true);
}

void torn_head(const Bytes &dirty, usize bytes, bool valid, bool renamed)
{
    Disk disk(dirty); Owner::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    CHECK(owner->prepare_rename(rename_update));
    disk.fail_write = 7; disk.cut = bytes;
    CHECK(!owner->commit());
    CHECK(owner->report().phase == Ext4OwnedPhase::failed);
    disk.crash();
    if (valid) { restart(disk.durable, renamed); return; }
    Disk next(disk.durable);
    Ext4MountedMetadata<1>::Registry next_registry;
    auto next_key = next_registry.register_device(next, 19); CHECK(next_key);
    auto mount = std::make_unique<Ext4MountedMetadata<1>>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(!mount->mount(next_registry, next_key.value(), {0, next.live.size() / 512}));
    CHECK(mount->report().phase == Ext4MountPhase::failed);
    CHECK(next.writes == 0 && next.flushes == 0 && next.durable == disk.durable);
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes clean((std::istreambuf_iterator<char>(input)), {});
    const Bytes original = relocation_fixture::with_notes_source(clean);
    CHECK(original.size() == 32 * 1024 * 1024);
    for (bool free_record : {false, true})
    {
    const Bytes dirty = dirty_empty(free_record ? relocation_fixture::with_free_record(original) : original);
    rejected(dirty, {2, 12, 1700000111, 123456789, "missing", "saved-note.txt"}, StatusCode::not_found);
    rejected(dirty, {2, 12, 1700000111, 123456789, "notes.txt", "notes.txt"}, StatusCode::already_exists);
    rejected(dirty, {2, 12, 1700000111, 1000000000, "notes.txt", "saved-note.txt"}, StatusCode::invalid_argument);
    rejected(dirty, {2, 13, 1700000111, 123456789, "notes.txt", "saved-note.txt"}, StatusCode::unsupported);
    for (usize index = 1; index <= 7; ++index) publication_fault(dirty, index, 0);
    for (usize index = 1; index <= 4; ++index) publication_fault(dirty, 0, index);
    for (usize index = 1; index <= 4; ++index) checkpoint_fault(dirty, index, 0);
    for (usize index = 1; index <= 4; ++index) checkpoint_fault(dirty, 0, index);
    torn_head(dirty, 1, true, false);
    for (usize bytes : {28U, 64U, 255U}) torn_head(dirty, bytes, false, false);
    for (usize bytes : {256U, 512U}) torn_head(dirty, bytes, true, true);
    }
    std::puts("PASS relocation before effect: free/slack each 4 benign refusals, 19 owner write/flush failures and 6 torn journal heads; scope=model");
}
