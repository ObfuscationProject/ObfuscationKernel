#include "ext4_owned_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <ok/posix/posix.hpp>
#include <memory>

using namespace owned_fixture;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL mounted rename no-op:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    w32(original, 1120, r32(original, 1120) | 4U); super_crc(original);
    Disk disk(original);
    Ext4MountedMetadata<1>::Registry registry;
    auto key = registry.register_device(disk, 71); CHECK(key);
    auto mount = std::make_unique<Ext4MountedMetadata<1>>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(mount->mount(registry, key.value(), {0, original.size() / 512}));
    auto *root = mount->root(); CHECK(root);
    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->create("/mnt", NodeType::directory) && vfs->mount("/mnt", *root));
    auto scheduler = std::make_unique<sched::Scheduler>();
    driver::ConsoleDriver console;
    auto service = std::make_unique<posix::PosixService>();
    CHECK(console.start() && service->initialize(*vfs, console, *scheduler));
    auto process = scheduler->create_user_process("rename-noop",
        arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
    CHECK(process && service->activate(process.value()));
    CHECK(service->set_credentials(user::root_credentials()));
    auto *seed = vfs->lookup("/mnt/seed"); CHECK(seed && seed->retain());
    CHECK(vfs->lookup("/mnt/alias") == seed);
    auto before = seed->query_metadata(); CHECK(before && before.value().link_count == 2);
    const Bytes snapshot = disk.live;
    const auto writes = disk.writes, flushes = disk.flushes, epoch = mount->report().epoch;
    testing::ext4_clock::reset();
    CHECK(service->renameat(posix::at_FDCWD, "/mnt/seed", posix::at_FDCWD, "/mnt/alias"));
    CHECK(service->rename("/mnt/alias", "/mnt/seed"));
    CHECK(service->rename("/mnt/seed", "/mnt/seed"));
    CHECK(service->rename("/mnt/missing", "/mnt/missing").code() == StatusCode::not_found);
    CHECK(service->rename("/mnt/seed/", "/mnt/seed").code() == StatusCode::not_directory);
    CHECK(service->set_credentials({1000, 1000, 1000, 1000, false}));
    CHECK(service->rename("/mnt/seed", "/mnt/alias").code() == StatusCode::denied);
    CHECK(service->rename("/mnt/seed", "/mnt/seed").code() == StatusCode::denied);
    CHECK(testing::ext4_clock::calls() == 0);
    CHECK(disk.live == snapshot && disk.durable == snapshot && disk.pending.empty());
    CHECK(disk.writes == writes && disk.flushes == flushes);
    CHECK(mount->report().phase == Ext4MountPhase::readable && mount->report().epoch == epoch);
    CHECK(vfs->lookup("/mnt/seed") == seed && vfs->lookup("/mnt/alias") == seed);
    const auto after = seed->query_metadata(); CHECK(after);
    CHECK(after.value().link_count == 2 && after.value().attributes.inode == before.value().attributes.inode);
    CHECK(after.value().attributes.change.seconds == before.value().attributes.change.seconds);
    CHECK(after.value().attributes.change.nanoseconds == before.value().attributes.change.nanoseconds);
    CHECK(seed->release());
    std::puts("PASS hosted mounted POSIX rename no-op: same name and aliases, no clock, writes or FLUSH, credentials preserved");
}
