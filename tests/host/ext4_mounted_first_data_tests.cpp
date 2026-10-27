#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <ok/posix/posix.hpp>
#include <memory>

using namespace owned_fixture;
namespace clock_model = ok::testing::ext4_clock;
using Mount = Ext4MountedMetadata<1>;

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL mounted first data:%d: %s\n", __LINE__, #expression); std::exit(1); \
} } while (false)

static time::RealtimeSample sample()
{ return {{1700000200, 123456789}, {200, 1}, 1, time::RealtimeOrigin::administrator_software}; }


struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount{Ext4MountedPolicy::ordered_data_dirty_noatime};
    Mount::Registry::Registration key{};
    explicit Mounted(const Bytes &image) : disk(image)
    {
        auto registered = registry.register_device(disk, 17); CHECK(registered);
        key = registered.value();
        CHECK(mount.mount(registry, key, {0, image.size() / 512}));
    }
    Node *root() { auto *node = mount.root(); CHECK(node); return node; }
};

struct Services
{
    sched::Scheduler scheduler;
    VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService service;
    explicit Services(Node &root)
    {
        CHECK(console.start() && service.initialize(vfs, console, scheduler));
        auto process = scheduler.create_user_process("mounted-create",
            arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
        CHECK(process && service.activate(process.value()));
        CHECK(service.set_credentials(user::root_credentials()));
        CHECK(vfs.create("/mnt", NodeType::directory) && vfs.mount("/mnt", root));
    }
};


int main(int argc, char **argv)
{
    CHECK(argc == 3); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes image((std::istreambuf_iterator<char>(input)), {});
    CHECK(image.size() == 32 * 1024 * 1024);
    const usize root_at = inode_offset(image, 2);
    w16(image, root_at, 0040777); inode_crc(image, 2, root_at);
    clock_model::set(sample());
    auto mounted = std::make_unique<Mounted>(image);
    auto services = std::make_unique<Services>(*mounted->root());
    CHECK(services->service.set_identity(1000, 1000));
    CHECK(services->service.umask(0027) == 0022);
    const auto fd = services->service.open("/mnt/new-note", posix::o_CREAT | posix::o_EXCL | posix::o_RDWR, 0666);
    CHECK(fd);
    const u32 free_before = r32(mounted->disk.live, 1036), inodes_before = r32(mounted->disk.live, 1040);
    const auto before = services->service.fstat(fd.value()); CHECK(before);
    CHECK(before.value().mode == 0100640 && before.value().uid == 1000 && before.value().gid == 1000 &&
          before.value().size == 0 && before.value().blocks == 0);
    auto file = mounted->root()->lookup_child("new-note"); CHECK(file);
    const auto writes = mounted->disk.writes, flushes = mounted->disk.flushes;
    const auto epoch = mounted->mount.report().epoch;
    constexpr std::string_view text = "Basic English note.\n";
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    CHECK(!file.value()->write(1010, bytes));
    CHECK(mounted->disk.writes == writes && mounted->disk.flushes == flushes && mounted->mount.report().epoch == epoch);
    clock_model::reset();
    CHECK(file.value()->write(7, bytes).code() == StatusCode::io_error);
    CHECK(mounted->mount.report().phase == Ext4MountPhase::readable && mounted->disk.writes == writes);
    clock_model::set(sample());
    const auto written = services->service.pwrite(fd.value(), bytes, 7);
    if (!written) std::fprintf(stderr, "first pwrite: %s\n", written.status().message().data());
    CHECK(written && written.value() == text.size());
    CHECK(mounted->mount.report().epoch == epoch + 1 && mounted->mount.report().phase == Ext4MountPhase::readable);
    auto after = services->service.fstat(fd.value()); CHECK(after);
    CHECK(after.value().attributes.inode == before.value().attributes.inode && after.value().size == 7 + text.size() && after.value().blocks == 2);
    CHECK(after.value().mode == before.value().mode && after.value().uid == 1000 && after.value().gid == 1000);
    CHECK(r32(mounted->disk.live, 1036) == free_before - 1 && r32(mounted->disk.live, 1040) == inodes_before);
    std::array<std::byte, 64> read{};
    const auto reread = services->service.pread(fd.value(), read, 0); CHECK(reread && reread.value() == after.value().size);
    for (usize at = 0; at < 7; ++at) CHECK(read[at] == std::byte{});
    for (usize at = 0; at < text.size(); ++at) CHECK(read[at + 7] == bytes[at]);
    CHECK(services->service.fsync(fd.value()));
    const auto directory = services->service.open("/mnt", posix::o_RDONLY); CHECK(directory);
    CHECK(services->service.fsync(directory.value()) && services->service.close(directory.value()));
    CHECK(services->service.close(fd.value()) && mounted->disk.live == mounted->disk.durable);
    // Reuse the same persistent mounted owner for a second creation/allocation;
    // cache generations and session/transaction serials must not restart.
    const auto second = services->service.open("/mnt/second-note",
        posix::o_CREAT | posix::o_EXCL | posix::o_RDWR, 0666);
    CHECK(second);
    const auto second_written = services->service.write(second.value(), bytes);
    CHECK(second_written && second_written.value() == bytes.size());
    CHECK(services->service.fsync(second.value()) && services->service.close(second.value()));
    CHECK(mounted->mount.report().epoch == epoch + 3);
    CHECK(r32(mounted->disk.live, 1036) == free_before - 2 && r32(mounted->disk.live, 1040) == inodes_before - 1);
    save(argv[2], mounted->disk.durable);
    services.reset();
    auto restarted = std::make_unique<Mounted>(mounted->disk.durable);
    const auto reopened = restarted->root()->lookup_child("new-note"); CHECK(reopened);
    const auto metadata = reopened.value()->query_metadata(); CHECK(metadata);
    CHECK(metadata.value().size == 7 + text.size() && metadata.value().blocks == 2 &&
          metadata.value().uid == 1000 && metadata.value().gid == 1000);
    auto contents = reopened.value()->read(0, 64); CHECK(contents && contents.value().size == 7 + text.size());
    for (usize at = 0; at < 7; ++at) CHECK(contents.value().data[at] == std::byte{});
    for (usize at = 0; at < text.size(); ++at) CHECK(contents.value().data[7 + at] == bytes[at]);
    const auto second_reopened = restarted->root()->lookup_child("second-note"); CHECK(second_reopened);
    const auto second_contents = second_reopened.value()->read(0, 64);
    CHECK(second_contents && second_contents.value().size == text.size());
    for (usize at = 0; at < text.size(); ++at) CHECK(second_contents.value().data[at] == bytes[at]);
    // Existing allocated-block writes retain their previous path and cannot
    // charge a second allocation to the newly created file.
    constexpr std::array<std::byte, 2> replacement{std::byte{'O'}, std::byte{'K'}};
    CHECK(reopened.value()->write(7, replacement));
    CHECK(r32(restarted->disk.live, 1036) == free_before - 2);
    CHECK(reopened.value()->query_metadata().value().size == 7 + text.size());
    CHECK(reopened.value()->truncate(0).code() == StatusCode::unsupported);
    CHECK(reopened.value()->write(1024, replacement).code() == StatusCode::unsupported);
    CHECK(restarted->mount.report().phase == Ext4MountPhase::readable);
    std::printf("PASS hosted mounted first data: UID1000 consecutive create/save, sparse pwrite, zero fill, fsync, restart and existing-data regression\n");
}
