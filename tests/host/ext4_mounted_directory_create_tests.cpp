#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <ok/posix/posix.hpp>
#include <memory>

using namespace owned_fixture;
namespace clock_model = ok::testing::ext4_clock;
using Mount = Ext4MountedMetadata<1>;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL mounted mkdir:%d: %s\n", __LINE__, #expression); std::exit(1); \
} } while (false)

namespace
{
struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount{Ext4MountedPolicy::ordered_data_dirty_noatime};
    explicit Mounted(const Bytes &image) : disk(image)
    {
        auto key = registry.register_device(disk, 17); CHECK(key);
        CHECK(mount.mount(registry, key.value(), {0, image.size() / 512}));
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
        auto process = scheduler.create_user_process("mounted-mkdir",
            arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
        CHECK(process && service.activate(process.value()));
        CHECK(service.set_credentials(user::root_credentials()));
        CHECK(vfs.create("/mnt", NodeType::directory) && vfs.mount("/mnt", root));
    }
};
void directory(Node &node, u32 mode, u32 uid, u32 gid, u32 parent)
{
    const auto metadata = node.query_metadata(); CHECK(metadata);
    CHECK(metadata.value().type == NodeType::directory && metadata.value().size == 1024 &&
          metadata.value().blocks == 2 && metadata.value().link_count == 2);
    CHECK(metadata.value().mode == (mode_directory | mode) &&
          metadata.value().uid == uid && metadata.value().gid == gid);
    auto dot = node.lookup_child_retained("."); CHECK(dot);
    CHECK(dot.value()->query_metadata().value().attributes.inode == metadata.value().attributes.inode);
    CHECK(dot.value()->release());
    auto dotdot = node.lookup_child_retained(".."); CHECK(dotdot);
    CHECK(dotdot.value()->query_metadata().value().attributes.inode == parent);
    CHECK(dotdot.value()->release());
}
Bytes dirty(Bytes image)
{ w32(image, 1120, r32(image, 1120) | 4U); super_crc(image); return image; }
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes clean((std::istreambuf_iterator<char>(input)), {});
    CHECK(clean.size() == 32 * 1024 * 1024);
    const usize bitmap = r32(clean, descriptor(clean, 0) + 4) * 1024;
    for (u32 inode : {14u, 15u})
    {
        CHECK(!(clean[bitmap + (inode - 1) / 8] & (1U << ((inode - 1) % 8))));
        std::fill_n(clean.begin() + inode_offset(clean, inode), 256, 0);
    }
    const auto image = dirty(clean);
    constexpr InitialNodeAttributes attributes{NodeType::directory, 02750, 0x10001, 0x20002};
    clock_model::reset();
    auto mounted = std::make_unique<Mounted>(image);
    auto *root = mounted->root();
    const auto root_before = root->query_metadata(); CHECK(root_before);
    const auto writes = mounted->disk.writes, flushes = mounted->disk.flushes;
    CHECK(root->create_child_retained("clockless", attributes).status().code() == StatusCode::io_error);
    CHECK(root->create_child_retained("seed", attributes).status().code() == StatusCode::already_exists);
    CHECK(root->create_child_retained("bad/name", attributes).status().code() == StatusCode::invalid_argument);
    CHECK(root->create_child_retained("unsupported", {NodeType::symlink, 0777, 0, 0}).status().code() == StatusCode::unsupported);
    CHECK(mounted->disk.writes == writes && mounted->disk.flushes == flushes);
    clock_model::set({{1700000200, 123456789}, {200, 1}, 1, time::RealtimeOrigin::administrator_software});
    auto created = root->create_child_retained("inbox", attributes); CHECK(created);
    directory(*created.value(), 02750, 0x10001, 0x20002, root_before.value().attributes.inode);
    const auto created_inode = created.value()->query_metadata().value().attributes.inode;
    CHECK(created.value()->query_metadata().value().attributes.modification.nanoseconds == 123456789);
    CHECK(root->query_metadata().value().link_count == root_before.value().link_count + 1);
    CHECK(created.value()->sync() && created.value()->release());
    auto reopened = root->lookup_child_retained("inbox"); CHECK(reopened);
    CHECK(reopened.value()->query_metadata().value().attributes.inode == created_inode);
    auto file = reopened.value()->create_child_retained("note", {NodeType::regular, 0640, 71, 72}); CHECK(file);
    CHECK(file.value()->query_metadata().value().type == NodeType::regular);
    CHECK(file.value()->release() && reopened.value()->release());
    auto services = std::make_unique<Services>(*root);
    CHECK(services->service.umask(0027) == 0022);
    CHECK(services->service.mkdir("/mnt/posix", 0777));
    auto stat = services->service.stat("/mnt/posix"); CHECK(stat);
    CHECK(stat.value().mode == 0040750 && stat.value().size == 1024 && stat.value().blocks == 2);
    CHECK(services->service.mkdir("/mnt/posix", 0777).code() == StatusCode::already_exists);
    CHECK(root->sync());
    save(argv[2], mounted->disk.durable);
    services.reset();
    auto restarted = std::make_unique<Mounted>(mounted->disk.durable);
    auto durable = restarted->root()->lookup_child_retained("inbox"); CHECK(durable);
    CHECK(durable.value()->query_metadata().value().attributes.inode == created_inode);
    auto durable_note = durable.value()->lookup_child_retained("note"); CHECK(durable_note);
    CHECK(durable_note.value()->query_metadata().value().mode == 0100640);
    CHECK(durable_note.value()->release() && durable.value()->release());
    restarted.reset(); mounted.reset();

    auto inherited_image = image;
    const usize root_offset = inode_offset(inherited_image, 2);
    w16(inherited_image, root_offset, 0042775); w16(inherited_image, root_offset + 24, 77);
    inode_crc(inherited_image, 2, root_offset);
    auto inherited = std::make_unique<Mounted>(inherited_image);
    auto inherited_services = std::make_unique<Services>(*inherited->root());
    CHECK(inherited_services->service.set_identity(1000, 77));
    CHECK(inherited_services->service.umask(0027) == 0022);
    CHECK(inherited_services->service.mkdir("/mnt/inherited", 0777));
    auto inherited_stat = inherited_services->service.stat("/mnt/inherited"); CHECK(inherited_stat);
    CHECK(inherited_stat.value().mode == 0042750 && inherited_stat.value().uid == 1000 && inherited_stat.value().gid == 77);
    inherited_services.reset(); inherited.reset();

    auto measured = std::make_unique<Mounted>(image);
    const usize trace_begin = measured->disk.trace.size();
    auto measure = measured->root()->create_child_retained("cut", attributes); CHECK(measure);
    CHECK(measure.value()->release());
    std::vector<char> calls;
    for (usize index = trace_begin; index < measured->disk.trace.size(); ++index)
        if (measured->disk.trace[index].operation == 'w' || measured->disk.trace[index].operation == 'f')
            calls.push_back(measured->disk.trace[index].operation);
    measured.reset();
    CHECK(calls.size() >= 12);
    usize old_states{}, new_states{};
    for (usize point = 0; point < calls.size(); ++point)
    {
        auto interrupted = std::make_unique<Mounted>(image);
        usize preceding_writes{}, preceding_flushes{};
        for (usize index = 0; index < point; ++index)
            calls[index] == 'w' ? ++preceding_writes : ++preceding_flushes;
        if (calls[point] == 'w') interrupted->disk.fail_write = interrupted->disk.writes + preceding_writes + 1;
        else interrupted->disk.fail_flush = interrupted->disk.flushes + preceding_flushes + 1;
        CHECK(!interrupted->root()->create_child_retained("cut", attributes));
        CHECK(interrupted->mount.report().phase == Ext4MountPhase::failed);
        interrupted->disk.crash();
        auto resumed = std::make_unique<Mounted>(interrupted->disk.durable);
        auto found = resumed->root()->lookup_child_retained("cut");
        if (found)
        {
            directory(*found.value(), 02750, 0x10001, 0x20002, 2);
            CHECK(resumed->root()->query_metadata().value().link_count == root_before.value().link_count + 1);
            CHECK(found.value()->release()); ++new_states;
        }
        else
        {
            CHECK(found.status().code() == StatusCode::not_found);
            CHECK(resumed->root()->query_metadata().value().link_count == root_before.value().link_count);
            ++old_states;
        }
    }
    CHECK(old_states && new_states);
    std::printf("MOUNTED_DIRECTORY_CREATE_PASS retained POSIX umask setgid nested-file restart cuts=%zu old=%zu new=%zu\n",
                calls.size(), old_states, new_states);
}
