#include "ext4_owned_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL mounted rename:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace
{
Bytes dirty_empty(Bytes image)
{
    w32(image, 1120, r32(image, 1120) | 4U);
    super_crc(image);
    return image;
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    Disk disk(dirty_empty(original));
    Ext4MountedMetadata<1>::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto mount = std::make_unique<Ext4MountedMetadata<1>>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(mount->mount(registry, key.value(), {0, disk.live.size() / 512}));
    auto *root = mount->root(); CHECK(root);
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->create("/mnt", NodeType::directory));
    CHECK(vfs->mount("/mnt", *root));
    auto *retained = vfs->lookup("/mnt/seed"); CHECK(retained && retained->retain());
    const auto before = retained->query_metadata(); CHECK(before);
    CHECK(before.value().attributes.inode == 12);
    testing::ext4_clock::reset();
    const usize original_writes = disk.writes, original_flushes = disk.flushes;
    const Bytes same_name_before = disk.live;
    CHECK(vfs->rename("/mnt/seed", "/mnt/seed"));
    CHECK(vfs->rename("/mnt/missing", "/mnt/missing").code() == StatusCode::not_found);
    CHECK(disk.live == same_name_before && disk.durable == same_name_before);
    CHECK(disk.writes == original_writes && disk.flushes == original_flushes);
    CHECK(mount->report().phase == Ext4MountPhase::readable && mount->report().epoch == 1);
    CHECK(vfs->rename("/mnt/seed", "/mnt/note").code() == StatusCode::io_error);
    CHECK(disk.writes == original_writes && disk.flushes == original_flushes);
    CHECK(mount->report().phase == Ext4MountPhase::readable && mount->report().epoch == 1);
    CHECK(root->rename("seed", *retained, "note").code() == StatusCode::unsupported);
    testing::ext4_clock::set({{1700000111, 123456789}, {0, 1}, 1,
                              time::RealtimeOrigin::administrator_software});
    CHECK(vfs->rename("/mnt/seed", "/mnt/note"));
    CHECK(!vfs->lookup("/mnt/seed"));
    CHECK(vfs->lookup("/mnt/note") == retained);
    CHECK(retained->name() == "note");
    const auto after = retained->query_metadata(); CHECK(after);
    CHECK(after.value().attributes.inode == before.value().attributes.inode);
    CHECK(after.value().attributes.change.seconds == 1700000111);
    CHECK(after.value().attributes.change.nanoseconds == 123456789);
    CHECK(mount->report().epoch == 2 && mount->report().phase == Ext4MountPhase::readable);
    const usize writes = disk.writes, flushes = disk.flushes;
    const Bytes no_op_before = disk.live;
    testing::ext4_clock::reset();
    CHECK(vfs->rename("/mnt/note", "/mnt/note"));
    CHECK(disk.live == no_op_before && disk.durable == no_op_before);
    const auto unchanged = retained->query_metadata(); CHECK(unchanged);
    CHECK(unchanged.value().attributes.change.seconds == after.value().attributes.change.seconds);
    CHECK(unchanged.value().attributes.change.nanoseconds == after.value().attributes.change.nanoseconds);
    CHECK(unchanged.value().attributes.modification.seconds == after.value().attributes.modification.seconds);
    CHECK(unchanged.value().attributes.modification.nanoseconds == after.value().attributes.modification.nanoseconds);
    CHECK(retained->name() == "note" && mount->report().epoch == 2);
    CHECK(disk.writes == writes && disk.flushes == flushes);
    CHECK(mount->report().phase == Ext4MountPhase::readable);
    testing::ext4_clock::set({{1700000111, 123456789}, {0, 1}, 1,
                              time::RealtimeOrigin::administrator_software});
    CHECK(vfs->rename("/mnt/note", "/mnt/text"));
    CHECK(vfs->lookup("/mnt/text") == retained && retained->name() == "text");
    CHECK(!vfs->lookup("/mnt/note"));
    CHECK(mount->report().epoch == 3);
    CHECK(retained->release());
    save(argv[2], disk.durable);
    CHECK(disk.live == disk.durable);
    Disk restarted(disk.durable);
    Ext4MountedMetadata<1>::Registry next_registry;
    auto next_key = next_registry.register_device(restarted, 19); CHECK(next_key);
    auto next_mount = std::make_unique<Ext4MountedMetadata<1>>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(next_mount->mount(next_registry, next_key.value(), {0, restarted.live.size() / 512}));
    auto *next_root = next_mount->root(); CHECK(next_root);
    CHECK(next_root->lookup_child("text"));
    CHECK(next_root->lookup_child("seed").status().code() == StatusCode::not_found);
    std::puts("PASS mounted VFS rename: two journalled mutations, retained inode, namespace renewal and restart");
}
