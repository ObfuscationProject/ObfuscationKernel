#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <memory>
#include <set>
#include <vector>
// The historical capacity fixture reaches private state; build with -fno-access-control.
#include <ok/fs/ext4/mount.hpp>
#include <ok/posix/posix.hpp>

using namespace owned_fixture;
namespace clock_model = ok::testing::ext4_clock;
using Mount = Ext4MountedMetadata<1>;

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL mounted create:%d: %s\n", __LINE__, #expression); std::exit(1); \
} } while (false)

static time::RealtimeSample sample()
{ return {{1700000200, 123456789}, {200, 1}, 1, time::RealtimeOrigin::administrator_software}; }

static Bytes dirty_empty(Bytes image)
{
    w32(image, 1120, r32(image, 1120) | 4U);
    super_crc(image);
    return image;
}

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

static void empty_file(Node &node, u32 mode, u32 uid, u32 gid)
{
    auto metadata = node.query_metadata(); CHECK(metadata);
    CHECK(metadata.value().type == NodeType::regular && metadata.value().size == 0 &&
          metadata.value().blocks == 0 && metadata.value().link_count == 1);
    CHECK(metadata.value().mode == (mode_regular | mode) &&
          metadata.value().uid == uid && metadata.value().gid == gid);
    auto read = node.read(0, 4); CHECK(read && read.value().size == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    // The SDK44 seed fixture retains deleted inode bodies in its first two
    // free slots. This bounded profile deliberately requires zeroed slots.
    const usize bitmap = r32(original, descriptor(original, 0) + 4) * 1024;
    for (u32 inode : {14u, 15u})
    {
        CHECK(!(original[bitmap + (inode - 1) / 8] & (1u << ((inode - 1) % 8))));
        std::fill_n(original.begin() + inode_offset(original, inode), 256, 0);
    }
    const auto image = dirty_empty(original);
    constexpr InitialNodeAttributes direct{NodeType::regular, 0640, 71, 72};
    clock_model::reset();
    auto mounted = std::make_unique<Mounted>(image);
    auto *root = mounted->root();
    const auto writes = mounted->disk.writes, flushes = mounted->disk.flushes;
    CHECK(root->create_child("clockless", direct).status().code() == StatusCode::io_error);
    CHECK(root->create_child("hello", direct).status().code() == StatusCode::io_error);
    CHECK(root->create_child("seed", direct).status().code() == StatusCode::already_exists);
    CHECK(root->create_child("bad/name", direct).status().code() == StatusCode::invalid_argument);
    CHECK(root->create_child("directory", {NodeType::directory, 0755, 0, 0}).status().code() ==
          StatusCode::io_error);
    CHECK(root->create_child("unsupported", {NodeType::symlink, 0777, 0, 0}).status().code() ==
          StatusCode::unsupported);
    CHECK(mounted->disk.writes == writes && mounted->disk.flushes == flushes);
    CHECK(mounted->mount.report().phase == Ext4MountPhase::readable);

    clock_model::set(sample());
    auto created = root->create_child("hello", direct); CHECK(created);
    CHECK(mounted->mount.report().phase == Ext4MountPhase::readable && mounted->mount.report().epoch == 2);
    CHECK(root->lookup_child("hello").value() == created.value());
    empty_file(*created.value(), 0640, 71, 72);
    CHECK(created.value()->query_metadata().value().attributes.change.seconds == 1700000200);
    CHECK(created.value()->query_metadata().value().attributes.modification.nanoseconds == 123456789);
    CHECK(root->sync() && created.value()->sync());
    CHECK(mounted->disk.live == mounted->disk.durable);

    auto services = std::make_unique<Services>(*root);
    CHECK(services->service.umask(0027) == 0022);
    auto descriptor = services->service.open("/mnt/posix", posix::o_CREAT | posix::o_EXCL | posix::o_RDWR, 0666);
    CHECK(descriptor);
    auto stat = services->service.fstat(descriptor.value()); CHECK(stat);
    CHECK(stat.value().mode == 0100640 && stat.value().uid == 0 && stat.value().gid == 0 &&
          stat.value().size == 0 && stat.value().blocks == 0);
    CHECK(services->service.fsync(descriptor.value()));
    auto directory = services->service.open("/mnt", posix::o_RDONLY); CHECK(directory);
    CHECK(services->service.fsync(directory.value()));
    CHECK(services->service.close(directory.value()) && services->service.close(descriptor.value()));
    const auto second_writes = mounted->disk.writes, second_flushes = mounted->disk.flushes;
    CHECK(services->service.open("/mnt/posix", posix::o_CREAT | posix::o_EXCL, 0666).status().code() ==
          StatusCode::already_exists);
    auto existing_fd = services->service.open("/mnt/posix", posix::o_CREAT | posix::o_RDWR, 0666);
    CHECK(existing_fd && services->service.close(existing_fd.value()));
    CHECK(mounted->disk.writes == second_writes && mounted->disk.flushes == second_flushes);
    CHECK(mounted->mount.report().phase == Ext4MountPhase::readable && mounted->mount.report().epoch == 3);
    save(argv[2], mounted->disk.durable);
    services.reset();

    auto restarted = std::make_unique<Mounted>(mounted->disk.durable);
    auto *reopened = restarted->root();
    auto first = reopened->lookup_child("hello"); CHECK(first);
    auto second = reopened->lookup_child("posix"); CHECK(second);
    empty_file(*first.value(), 0640, 71, 72);
    empty_file(*second.value(), 0640, 0, 0);
    CHECK(reopened->lookup_child("clockless").status().code() == StatusCode::not_found);
    restarted.reset();
    mounted.reset();

    // VFS must pass final umask and setgid inheritance to the provider before
    // it publishes a node; the provider has no post-create ownership repair.
    auto inherited_image = image;
    const usize root_inode = inode_offset(inherited_image, 2);
    w16(inherited_image, root_inode, 0040000 | 02775);
    w16(inherited_image, root_inode + 24, 77);
    w16(inherited_image, root_inode + 122, 0);
    inode_crc(inherited_image, 2, root_inode);
    auto inherited = std::make_unique<Mounted>(inherited_image);
    auto inherited_services = std::make_unique<Services>(*inherited->root());
    CHECK(inherited_services->service.set_identity(1000, 77));
    CHECK(inherited_services->service.umask(0027) == 0022);
    auto inherited_fd = inherited_services->service.open("/mnt/inherited",
        posix::o_CREAT | posix::o_EXCL | posix::o_RDWR, 0666);
    CHECK(inherited_fd);
    auto inherited_stat = inherited_services->service.fstat(inherited_fd.value()); CHECK(inherited_stat);
    CHECK(inherited_stat.value().mode == 0100640 && inherited_stat.value().uid == 1000 &&
          inherited_stat.value().gid == 77);
    CHECK(inherited_services->service.close(inherited_fd.value()));
    inherited_services.reset();
    inherited.reset();

    auto sync_fault = std::make_unique<Mounted>(image);
    auto sync_file = sync_fault->root()->create_child("sync", direct); CHECK(sync_file);
    sync_fault->disk.fail_flush = sync_fault->disk.flushes + 1;
    CHECK(!sync_file.value()->sync());
    CHECK(sync_fault->mount.report().phase == Ext4MountPhase::failed);
    const auto sync_writes = sync_fault->disk.writes, sync_flushes = sync_fault->disk.flushes;
    CHECK(sync_fault->mount.root() == nullptr);
    CHECK(sync_fault->disk.writes == sync_writes && sync_fault->disk.flushes == sync_flushes);
    sync_fault.reset();

    // The node cache holds 128 identities. Unreferenced identities that were
    // never handed out raw are evicted, so one mount creates and reopens many
    // more files; an open descriptor keeps its node resident throughout.
    auto many = std::make_unique<Mounted>(image);
    auto many_services = std::make_unique<Services>(*many->root());
    auto &many_posix = many_services->service;
    auto kept = many_posix.open("/mnt/kept", posix::o_CREAT | posix::o_EXCL | posix::o_RDWR, 0644);
    CHECK(kept);
    const auto kept_inode = many_posix.fstat(kept.value()).value().attributes.inode;
    constexpr usize many_directories = 8, many_files = 400;
    for (usize directory = 0; directory < many_directories; ++directory)
    {
        char path[32]; std::snprintf(path, sizeof(path), "/mnt/d%zu", directory);
        CHECK(many_posix.mkdir(path, 0755));
    }
    std::vector<u64> many_inodes;
    for (usize index = 0; index < many_files; ++index)
    {
        char path[32]; std::snprintf(path, sizeof(path), "/mnt/d%zu/f%zu", index % many_directories, index);
        auto fd = many_posix.open(path, posix::o_CREAT | posix::o_EXCL | posix::o_RDWR, 0644);
        CHECK(fd);
        many_inodes.push_back(many_posix.fstat(fd.value()).value().attributes.inode);
        CHECK(many_posix.close(fd.value()));
    }
    CHECK(many->mount.lifetime_.active_records() <= many->mount.nodes_.size());
    for (usize index = 0; index < many_files; ++index)
    {
        char path[32]; std::snprintf(path, sizeof(path), "/mnt/d%zu/f%zu", index % many_directories, index);
        auto fd = many_posix.open(path, posix::o_RDONLY);
        CHECK(fd);
        CHECK(many_posix.fstat(fd.value()).value().attributes.inode == many_inodes[index]);
        CHECK(many_posix.close(fd.value()));
    }
    CHECK(std::set<u64>(many_inodes.begin(), many_inodes.end()).size() == many_files);
    {
        // The first and a block-aligned next data block are admitted with more
        // than 128 inodes in use (each proves the block against every inode).
        auto data = many_posix.open("/mnt/d7/f399", posix::o_RDWR); CHECK(data);
        const std::array<std::byte, 1024> payload{std::byte{0x5a}};
        CHECK(many_posix.write(data.value(), payload).value() == payload.size());
        CHECK(many_posix.write(data.value(), payload).value() == payload.size());
        CHECK(many_posix.fstat(data.value()).value().size == 2 * payload.size());
        CHECK(many_posix.close(data.value()));
    }
    CHECK(many_posix.fstat(kept.value()).value().attributes.inode == kept_inode);
    CHECK(many_posix.close(kept.value()));
    CHECK(many->mount.report().phase == Ext4MountPhase::readable);
    CHECK(many->root()->sync());
    save(std::string(argv[2]) + ".many", many->disk.durable);
    many_services.reset();
    many.reset();

    auto pressure = std::make_unique<Mounted>(image);
    auto *pressure_root = pressure->root();
    for (usize index = 1; index < pressure->mount.nodes_.size(); ++index)
        pressure->mount.nodes_[index].inode_ = static_cast<u32>(100000 + index);
    const auto pressure_writes = pressure->disk.writes, pressure_flushes = pressure->disk.flushes;
    CHECK(pressure_root->create_child("capacity", direct).status().code() == StatusCode::no_memory);
    CHECK(pressure->disk.writes == pressure_writes && pressure->disk.flushes == pressure_flushes);
    CHECK(pressure->mount.report().phase == Ext4MountPhase::readable);
    pressure.reset();

    auto fault = std::make_unique<Mounted>(image);
    auto *fault_root = fault->root();
    fault->disk.fail_write = fault->disk.writes + 1;
    CHECK(!fault_root->create_child("fault", direct));
    CHECK(fault->mount.report().phase == Ext4MountPhase::failed);
    CHECK(fault_root->lookup_child("fault").status().code() == StatusCode::denied);
    CHECK(fault->mount.nodes_[1].inode_ == 0);
    fault->disk.crash();
    auto recovered = std::make_unique<Mounted>(fault->disk.durable);
    CHECK(recovered->root()->lookup_child("fault").status().code() == StatusCode::not_found);
    recovered.reset();
    fault.reset();

    // Every transport-call prefix is restarted through a fresh mounted owner.
    // Successful recovery must expose either the old namespace or the complete
    // new empty inode, never a partial directory/bitmap/counter combination.
    auto measured = std::make_unique<Mounted>(image);
    const usize trace_begin = measured->disk.trace.size();
    CHECK(measured->root()->create_child("cut", direct));
    std::vector<char> calls;
    for (usize index = trace_begin; index < measured->disk.trace.size(); ++index)
        if (measured->disk.trace[index].operation == 'w' || measured->disk.trace[index].operation == 'f')
            calls.push_back(measured->disk.trace[index].operation);
    CHECK(calls.size() >= 8);
    measured.reset();
    usize old_states{}, new_states{};
    for (usize point = 0; point < calls.size(); ++point)
    {
        auto interrupted = std::make_unique<Mounted>(image);
        usize preceding_writes{}, preceding_flushes{};
        for (usize index = 0; index < point; ++index)
            calls[index] == 'w' ? ++preceding_writes : ++preceding_flushes;
        if (calls[point] == 'w')
            interrupted->disk.fail_write = interrupted->disk.writes + preceding_writes + 1;
        else
            interrupted->disk.fail_flush = interrupted->disk.flushes + preceding_flushes + 1;
        auto *interrupted_root = interrupted->root();
        CHECK(!interrupted_root->create_child("cut", direct));
        CHECK(interrupted->mount.report().phase == Ext4MountPhase::failed);
        CHECK(interrupted->mount.nodes_[1].inode_ == 0);
        interrupted->disk.crash();
        auto resumed = std::make_unique<Mounted>(interrupted->disk.durable);
        auto found = resumed->root()->lookup_child("cut");
        if (found) { empty_file(*found.value(), 0640, 71, 72); ++new_states; }
        else { CHECK(found.status().code() == StatusCode::not_found); ++old_states; }
    }
    CHECK(old_states && new_states);
    std::printf("PASS mounted empty create: POSIX, fsync, restart, eviction, capacity, quarantine, cuts=%zu old=%zu new=%zu\n",
                calls.size(), old_states, new_states);
}
