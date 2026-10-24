#include "ext4_owned_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL rename after effect:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace
{
using Owner = Ext4OwnedTransaction<1, 8>;
using Mount = Ext4MountedMetadata<1>;
constexpr Ext4RenameUpdate rename_update{2, 12, 1700000111, 123456789, "seed", "note"};
std::filesystem::path artifact_root;

Bytes dirty_empty(Bytes image)
{
    w32(image, 1120, r32(image, 1120) | 4U);
    super_crc(image);
    return image;
}

void restart(const Bytes &durable, bool renamed)
{
    Disk disk(durable); Mount::Registry registry;
    auto key = registry.register_device(disk, 19); CHECK(key);
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    const auto mounted = mount->mount(registry, key.value(), {0, durable.size() / 512});
    if (!mounted) std::fprintf(stderr, "restart mount status=%u reason=%.*s writes=%zu flushes=%zu\n",
        unsigned(mounted.code()), int(mounted.message().size()), mounted.message().data(), disk.writes, disk.flushes);
    CHECK(mounted);
    auto *root = mount->root(); CHECK(root);
    auto present = root->lookup_child(renamed ? "note" : "seed"); CHECK(present);
    CHECK(present.value()->query_metadata().value().attributes.inode == 12);
    CHECK(root->lookup_child(renamed ? "seed" : "note").status().code() == StatusCode::not_found);
    CHECK(disk.live == disk.durable);
}

Bytes published_rename(const Bytes &dirty)
{
    Disk disk(dirty); Owner::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    CHECK(owner->prepare_rename(rename_update));
    CHECK(owner->commit());
    CHECK(disk.live == disk.durable && disk.writes == 7 && disk.flushes == 4);
    return disk.durable;
}

void recovery_fault(const Bytes &published, bool write, bool after, usize index)
{
    Disk disk(published);
    Ext4RecoveryCompletion<4, 2> completion(false, true, Ext4ReplayPolicy::directory_rename);
    if (write)
    {
        if (after) disk.fail_write_after = index;
        else disk.fail_write = index;
    }
    else
    {
        if (after) disk.fail_flush_after = index;
        else disk.fail_flush = index;
    }
    const auto status = completion.run(disk, {0, published.size() / 512});
    CHECK(!status && status.code() == StatusCode::io_error);
    CHECK(completion.report().phase == Ext4CompletionPhase::failed);
    CHECK(completion.report().write_attempts == (write ? index :
          index == 1 ? 0 : index == 2 ? 2 : index == 3 ? 3 : 4));
    CHECK(completion.report().flush_attempts == (write ?
          index <= 2 ? 1 : index == 3 ? 2 : 3 : index));
    if (!write && index == 2)
    {
        CHECK(completion.report().failed_phase == Ext4CompletionPhase::directory_table_flush);
        const u32 directory_block = r32(published, inode_offset(published, 2) + 60);
        CHECK(std::none_of(disk.submitted.begin(), disk.submitted.end(),
             [directory_block](const Write &item) { return item.sector == u64(directory_block) * 2; }));
    }
    const usize reads = disk.reads, writes = disk.writes, flushes = disk.flushes;
    CHECK(completion.run(disk, {0, published.size() / 512}).code() == StatusCode::already_exists);
    CHECK(disk.reads == reads && disk.writes == writes && disk.flushes == flushes);
    const Bytes uncertain = disk.durable;
    save(artifact_root / (std::string("recovery-") + (write ? "write-" : "flush-") +
          (after ? "after-" : "before-") + std::to_string(index) + ".img"), uncertain);
    disk.crash();
    restart(disk.durable, true);
}

void owner_quarantine(Owner &owner, Owner::Registry &registry,
                      Owner::Registry::Registration key, Disk &disk)
{
    CHECK(owner.report().phase == Ext4OwnedPhase::failed);
    CHECK(owner.buffers().state == MetadataOwnerState::failed);
    auto record = registry.inspect(key); CHECK(record);
    CHECK(record.value().state == block::DeviceState::quarantined);
    const usize reads = disk.reads, writes = disk.writes, flushes = disk.flushes;
    Owner::Pin pin;
    CHECK(!owner.pin(1, pin));
    CHECK(!owner.commit());
    CHECK(!owner.checkpoint());
    CHECK(!owner.close());
    CHECK(!registry.detach(key));
    CHECK(disk.reads == reads && disk.writes == writes && disk.flushes == flushes);
}

void owner_fault(const Bytes &dirty, bool checkpoint, bool write, usize index)
{
    Disk disk(dirty); Owner::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    CHECK(owner->prepare_rename(rename_update));
    if (checkpoint) CHECK(owner->commit());
    if (write) disk.fail_write_after = disk.writes + index;
    else disk.fail_flush_after = disk.flushes + index;
    const auto status = checkpoint ? owner->checkpoint() : owner->commit();
    CHECK(!status && status.code() == StatusCode::io_error);
    owner_quarantine(*owner, registry, key.value(), disk);
    const Bytes uncertain = disk.durable;
    save(artifact_root / (std::string("owner-") + (checkpoint ? "checkpoint" : "publication") +
                          (write ? "-write-" : "-flush-") + std::to_string(index) + ".img"), uncertain);
    disk.crash();
    CHECK(disk.durable == uncertain && disk.live == uncertain);
    std::fprintf(stderr, "owner checkpoint=%d write=%d index=%zu\n", int(checkpoint), int(write), index);
    restart(disk.durable, checkpoint || (write ? index == 7 : index == 4));
}

void mounted_fault(const Bytes &dirty, bool write, usize index)
{
    Disk disk(dirty); Mount::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(mount->mount(registry, key.value(), {0, dirty.size() / 512}));
    auto *root = mount->root(); CHECK(root);
    testing::ext4_clock::set({{1700000111, 123456789}, {0, 1}, 1,
                              time::RealtimeOrigin::administrator_software});
    if (write) disk.fail_write_after = disk.writes + index;
    else disk.fail_flush_after = disk.flushes + index;
    const auto status = root->rename("seed", *root, "note");
    CHECK(!status && status.code() == StatusCode::io_error);
    CHECK(mount->report().phase == Ext4MountPhase::failed);
    CHECK(mount->buffers().state == MetadataOwnerState::failed);
    auto record = registry.inspect(key.value()); CHECK(record);
    CHECK(record.value().state == block::DeviceState::quarantined);
    const usize reads = disk.reads, writes = disk.writes, flushes = disk.flushes;
    CHECK(root->lookup_child("seed").status().code() == StatusCode::denied);
    CHECK(root->lookup_child("note").status().code() == StatusCode::denied);
    CHECK(!root->list() && !root->sync());
    CHECK(!root->rename("seed", *root, "note"));
    CHECK(!mount->close() && !mount->freeze_clean() && !registry.detach(key.value()));
    CHECK(disk.reads == reads && disk.writes == writes && disk.flushes == flushes);
    const Bytes uncertain = disk.durable;
    save(artifact_root / (std::string("mounted-") + (write ? "write-" : "flush-") +
                          std::to_string(index) + ".img"), uncertain);
    disk.crash();
    CHECK(disk.durable == uncertain && disk.live == uncertain);
    std::fprintf(stderr, "mounted write=%d index=%zu\n", int(write), index);
    restart(disk.durable, write ? index >= 7 : index >= 4);
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    artifact_root = argv[2];
    CHECK(std::filesystem::create_directories(artifact_root));
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    const Bytes dirty = dirty_empty(original);
    for (usize index = 1; index <= 7; ++index) owner_fault(dirty, false, true, index);
    for (usize index = 1; index <= 4; ++index) owner_fault(dirty, false, false, index);
    for (usize index = 1; index <= 4; ++index) owner_fault(dirty, true, true, index);
    for (usize index = 1; index <= 4; ++index) owner_fault(dirty, true, false, index);
    for (usize index = 1; index <= 11; ++index) mounted_fault(dirty, true, index);
    for (usize index = 1; index <= 8; ++index) mounted_fault(dirty, false, index);
    const Bytes published = published_rename(dirty);
    for (bool after : {false, true})
    {
        for (usize index = 1; index <= 4; ++index) recovery_fault(published, true, after, index);
        for (usize index = 1; index <= 4; ++index) recovery_fault(published, false, after, index);
    }
    std::puts("PASS rename after effect: 19 owner, 19 mounted and 16 direct recovery I/O faults restart");
}
