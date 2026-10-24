#include "ext4_mounted_support.hpp"
#include "ext4_mounted_retention_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL mounted lifetime:%d: %s\n", __LINE__, #expression); std::exit(1); \
} } while (false)

using Mount = Ext4MountedMetadata<1>;

struct MoveOnlyPayload
{
    int value{};
    MoveOnlyPayload() = default;
    MoveOnlyPayload(const MoveOnlyPayload &) = delete;
    MoveOnlyPayload &operator=(const MoveOnlyPayload &) = delete;
    MoveOnlyPayload(MoveOnlyPayload &&) = default;
    MoveOnlyPayload &operator=(MoveOnlyPayload &&) = default;
};
struct NonDefaultPayload
{
    explicit NonDefaultPayload(int) {}
};
static_assert(std::constructible_from<Result<MoveOnlyPayload>, std::in_place_t>);
static_assert(!std::is_copy_constructible_v<Result<MoveOnlyPayload>>);
static_assert(!std::constructible_from<Result<NonDefaultPayload>, std::in_place_t>);

static void in_place_results()
{
    Result<DirectoryListing> listing{std::in_place}; CHECK(listing && listing.value().count == 0);
    for (const auto &entry : listing.value().entries)
        CHECK(entry.name.empty() && entry.metadata.size == 0);
    Result<MoveOnlyPayload> payload{std::in_place}; CHECK(payload && payload.value().value == 0);
    payload.value().value = 17;
    Result<MoveOnlyPayload> moved{std::move(payload)}; CHECK(moved && moved.value().value == 17);
    Result<int> value{41}; CHECK(value && value.value() == 41);
    const auto failure = Status::io_error("in-place Result original error");
    Result<int> error{failure}; CHECK(!error && error.status().code() == failure.code() &&
                                    error.status().message() == failure.message());
    std::puts("PASS in-place Result: default_payload=1 move_only=1 non_default_constraint=1 original_constructors_preserved=1");
}

struct Mounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount;
    Mount::Registry::Registration key{};

    explicit Mounted(const Bytes &image, Ext4MountedPolicy policy = Ext4MountedPolicy::maintenance_only)
        : disk(image), mount(policy)
    {
        auto registered = registry.register_device(disk, 17); CHECK(registered);
        key = registered.value();
        CHECK(mount.mount(registry, key, {0, image.size() / 512}));
    }

    Node *root() { auto *node = mount.root(); CHECK(node); return node; }
};

static Bytes dirty_empty(Bytes image)
{
    w32(image, 1120, r32(image, 1120) | 4U);
    super_crc(image);
    return image;
}

struct ListingMounted
{
    Disk disk;
    Mount::Registry registry;
    Mount mount{Ext4MountedPolicy::maintenance_only};
    VirtualFileSystem vfs;
    VfsMountHandle handle{};

    explicit ListingMounted(const Bytes &image) : disk(image)
    {
        const auto registered = registry.register_device(disk, 17); CHECK(registered);
        CHECK(mount.mount(registry, registered.value(), {0, image.size() / 512}));
        const auto published = publish_owned_root(mount, vfs); CHECK(published);
        handle = published.value();
    }

    void close()
    {
        CHECK(vfs.detach(handle));
        CHECK(mount.begin_drain());
        CHECK(mount.close());
        CHECK(disk.writes == 0 && disk.flushes == 0);
    }
};

// Retain the original allocated inodes and construct a checked directory with
// hard links to seed. The capacity refusal is reached through actual media
// parsing, rather than by changing the provider's listing capacity.
static Bytes hard_link_directory(Bytes image, usize entries)
{
    CHECK(entries == max_child_nodes || entries == max_child_nodes + 1);
    const usize size = block_size(image), inode = inode_offset(image, 2);
    const usize block = r32(image, inode + 60) * size;
    CHECK(size == 1024 && block + size <= image.size());
    std::fill_n(image.begin() + block, size, 0);
    usize position = block;
    const usize tail = block + size - 12;
    auto append = [&](u32 number, u8 type, std::string_view name, bool last = false) {
        const usize minimum = (8 + name.size() + 3) & ~usize{3};
        CHECK(position + minimum <= tail);
        const usize length = last ? tail - position : minimum;
        w32(image, position, number); w16(image, position + 4, u16(length));
        image[position + 6] = u8(name.size()); image[position + 7] = type;
        std::copy(name.begin(), name.end(), image.begin() + position + 8);
        position += length;
    };
    append(2, 2, "."); append(2, 2, ".."); append(11, 2, "lost+found");
    const usize links = entries - 1;
    append(12, 1, "seed");
    for (usize index = 1; index < links; ++index)
    {
        const std::array<char, 3> name{'f', char('0' + (index - 1) / 10), char('0' + (index - 1) % 10)};
        append(12, 1, {name.data(), name.size()}, index + 1 == links);
    }
    CHECK(position == tail);
    w16(image, tail + 4, 12); image[tail + 7] = 0xde;
    directory_crc(image, 2, block);
    const usize seed_inode = inode_offset(image, 12);
    w16(image, seed_inode + 26, u16(links)); inode_crc(image, 12, seed_inode);
    return image;
}

static void checked_list_results(const Bytes &original)
{
    {
        auto mounted = std::make_unique<ListingMounted>(original);
        const auto empty = mounted->vfs.list("/lost+found"); CHECK(empty && empty.value().count == 0);
        for (const auto &entry : empty.value().entries) CHECK(entry.name.empty());
        auto first = mounted->vfs.list("/"); CHECK(first && first.value().count > 0);
        const auto second = mounted->vfs.list("/"); CHECK(second && second.value().count == first.value().count);
        const auto original_name = second.value().entries[0].name;
        CHECK(first.value().entries[0].name.assign("private-result"));
        first.value().entries[0].metadata.uid = 999;
        const auto again = mounted->vfs.list("/"); CHECK(again && again.value().count == second.value().count);
        CHECK(second.value().entries[0].name.view() == original_name.view());
        CHECK(again.value().entries[0].name.view() == original_name.view());
        CHECK(again.value().entries[0].metadata.uid == second.value().entries[0].metadata.uid);
        mounted->close();
    }
    {
        auto mounted = std::make_unique<ListingMounted>(original);
        const auto listed = mounted->vfs.list("/"); CHECK(listed && listed.value().count > 0);
        const auto metadata = mounted->vfs.stat("/seed"); CHECK(metadata);
        bool found = false;
        for (usize index = 0; index < listed.value().count; ++index)
        {
            const auto &entry = listed.value().entries[index];
            CHECK(entry.name.view() != "." && entry.name.view() != "..");
            if (entry.name.view() == "seed")
            {
                found = true;
                CHECK(entry.metadata.type == metadata.value().type);
                CHECK(entry.metadata.size == metadata.value().size);
                CHECK(entry.metadata.attributes.inode == 12);
            }
        }
        CHECK(found);
        if (listed.value().count < max_child_nodes)
            CHECK(listed.value().entries[listed.value().count].name.empty());
        const auto content = mounted->vfs.read_file("/seed"); CHECK(content);
        const usize seed_inode = inode_offset(original, 12), block = r32(original, seed_inode + 60) * block_size(original);
        CHECK(content.value().size == std::min<usize>(r32(original, seed_inode + 4), max_file_data));
        CHECK(block + content.value().size <= original.size());
        for (usize index = 0; index < content.value().size; ++index)
            CHECK(content.value().data[index] == std::byte{original[block + index]});
        CHECK(!mounted->mount.report().reading);
        mounted->close();
    }
    for (usize count : {max_child_nodes, max_child_nodes + 1})
    {
        auto mounted = std::make_unique<ListingMounted>(hard_link_directory(original, count));
        auto root = mounted->vfs.resolve({}, "/"); CHECK(root);
        const auto listed = root.value().node()->list();
        if (count == max_child_nodes)
        {
            CHECK(listed && listed.value().count == max_child_nodes);
            for (usize index = 1; index < listed.value().count; ++index)
            {
                CHECK(listed.value().entries[index].metadata.attributes.inode == 12);
                CHECK(listed.value().entries[index].metadata.link_count == max_child_nodes - 1);
            }
        }
        else CHECK(!listed && listed.status().code() == StatusCode::overflow &&
                   listed.status().message() == "mounted directory capacity");
        CHECK(mounted->mount.report().phase == Ext4MountPhase::readable && !mounted->mount.report().reading);
        const auto held = mounted->mount.lifetime_snapshot(2); CHECK(held);
        CHECK(held.value().holds[static_cast<usize>(Ext4InodeHold::io)] == 0);
        CHECK(held.value().holds[static_cast<usize>(Ext4InodeHold::descriptor)] == 1);
        CHECK(root.value().release());
        const auto drained = mounted->mount.lifetime_snapshot(2); CHECK(drained && drained.value().total() == 0);
        mounted->close();
    }
    {
        auto mounted = std::make_unique<ListingMounted>(original);
        auto root = mounted->vfs.resolve({}, "/"); CHECK(root);
        bool reentered = false;
        mounted->disk.hook = [&](char operation) {
            if (operation != 'r' || reentered) return;
            reentered = true;
            CHECK(mounted->mount.report().reading);
            const usize reads = mounted->disk.reads;
            const auto nested = root.value().node()->list();
            CHECK(!nested && nested.status().code() == StatusCode::busy);
            CHECK(mounted->disk.reads == reads);
        };
        CHECK(root.value().node()->list() && reentered);
        mounted->disk.hook = {};
        mounted->disk.fail_read = mounted->disk.reads + 1;
        const auto failed = root.value().node()->list();
        CHECK(!failed && failed.status().code() == mounted->disk.read_error.code() &&
              failed.status().message() == mounted->disk.read_error.message());
        CHECK(mounted->mount.report().phase == Ext4MountPhase::failed && !mounted->mount.report().reading);
        const auto held = mounted->mount.lifetime_snapshot(2); CHECK(held);
        CHECK(held.value().holds[static_cast<usize>(Ext4InodeHold::io)] == 0);
        CHECK(root.value().release());
        CHECK(mounted->vfs.detach(mounted->handle));
        CHECK(mounted->disk.writes == 0 && mounted->disk.flushes == 0);
    }
    std::puts("PASS mounted listing: empty=1 independent_results=1 checked_content=1 capacity64=1 overflow65=1 original_media_failure=1 reentrant_busy=1 io_release=1 admission_reset=1 owned_drain=1");
}

// Isolate the read-operation tail with an intentionally inconsistent pending
// marker over a genuine checked Node. This is model fault injection, not an
// authentic orphan/unlink execution or a durability claim.
static void list_cleanup_priority(const Bytes &original)
{
    for (bool benign_failure : {false, true})
    {
        auto mounted = std::make_unique<ListingMounted>(benign_failure ?
            hard_link_directory(original, max_child_nodes + 1) : original);
        auto victim = mounted->mount.node(12, "seed"); CHECK(victim);
        auto root = mounted->vfs.resolve({}, "/"); CHECK(root);
        CHECK(!mounted->mount.pending_orphan_);
        mounted->mount.pending_orphan_ = static_cast<Mount::MountedNode *>(victim.value());
        const usize reads = mounted->disk.reads;
        const auto cleanup = mounted->mount.drain_pending_orphan();
        CHECK(!cleanup && cleanup.code() == StatusCode::fault &&
              cleanup.message() == "mounted orphan workspace or identity changed");
        CHECK(mounted->disk.reads == reads);
        const auto listed = root.value().node()->list(); CHECK(!listed);
        if (benign_failure)
            CHECK(listed.status().code() == StatusCode::overflow && listed.status().message() == "mounted directory capacity");
        else CHECK(listed.status().code() == cleanup.code() && listed.status().message() == cleanup.message());
        CHECK(mounted->mount.report().phase == Ext4MountPhase::readable && !mounted->mount.report().reading);
        const auto held = mounted->mount.lifetime_snapshot(2); CHECK(held);
        CHECK(held.value().holds[static_cast<usize>(Ext4InodeHold::io)] == 0);
        mounted->mount.pending_orphan_ = nullptr;
        CHECK(root.value().release());
        mounted->close();
    }
    std::puts("PASS mounted listing cleanup priority: private_tail_faults=2 benign_list_error_preserved=1 successful_list_reports_cleanup_error=1 no_reclaim_io=1");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    in_place_results();
    checked_list_results(original);
    list_cleanup_priority(original);

    auto mounted = std::make_unique<Mounted>(original);
    auto *root = mounted->root();
    auto *seed = root->lookup_child("seed").value();
    auto snapshot = mounted->mount.lifetime_snapshot(12); CHECK(snapshot);
    CHECK(snapshot.value().key.mount == &mounted->mount);
    CHECK(snapshot.value().key.session == mounted->mount.buffers().session_serial);
    CHECK(snapshot.value().key.generation == r32(original, inode_offset(original, 12) + 100));
    CHECK(snapshot.value().total() == 0);

    auto vfs = std::make_unique<VirtualFileSystem>();
    CHECK(vfs->create("/mnt", NodeType::directory));
    CHECK(vfs->mount("/mnt", *root));
    auto first = vfs->resolve({}, "/mnt/seed"); CHECK(first);
    auto second = vfs->resolve({}, "/mnt/seed"); CHECK(second);
    CHECK(first.value().node() == seed && second.value().node() == seed);
    snapshot = mounted->mount.lifetime_snapshot(12); CHECK(snapshot);
    CHECK(snapshot.value().holds[static_cast<usize>(Ext4InodeHold::descriptor)] == 2);
    CHECK(first.value().release());

    bool saw_in_flight = false;
    mounted->disk.hook = [&](char operation) {
        if (operation != 'r') return;
        const auto during = mounted->mount.lifetime_snapshot(12); CHECK(during);
        CHECK(during.value().holds[static_cast<usize>(Ext4InodeHold::io)] == 1);
        if (!saw_in_flight)
        {
            CHECK(during.value().holds[static_cast<usize>(Ext4InodeHold::descriptor)] == 1);
            CHECK(second.value().release());
            saw_in_flight = true;
        }
    };
    auto read = seed->read(0, 8); CHECK(read && saw_in_flight);
    mounted->disk.hook = {};
    snapshot = mounted->mount.lifetime_snapshot(12); CHECK(snapshot && snapshot.value().total() == 0);
    CHECK(second.value().release()); // A consumed PathReference is harmless.

    const usize reads = mounted->disk.reads;
    for (usize index = 0; index < 512; ++index) CHECK(seed->retain());
    CHECK(seed->retain().code() == StatusCode::no_memory);
    auto exhausted = vfs->resolve({}, "/mnt/seed");
    CHECK(!exhausted && exhausted.status().code() == StatusCode::no_memory);
    CHECK(mounted->disk.reads == reads); // Refuse before a new inode read or exposed PathReference.
    snapshot = mounted->mount.lifetime_snapshot(12); CHECK(snapshot);
    CHECK(snapshot.value().holds[static_cast<usize>(Ext4InodeHold::descriptor)] == 512);
    CHECK(snapshot.value().holds[static_cast<usize>(Ext4InodeHold::io)] == 0);
    CHECK(mounted->mount.report().phase == Ext4MountPhase::readable);
    for (usize index = 0; index < 512; ++index) CHECK(seed->release());
    CHECK(seed->release().code() == StatusCode::bad_descriptor);
    snapshot = mounted->mount.lifetime_snapshot(12); CHECK(snapshot && snapshot.value().total() == 0);

    auto fault = std::make_unique<Mounted>(original);
    auto *fault_seed = fault->root()->lookup_child("seed").value();
    CHECK(fault_seed->retain());
    fault->disk.fail_read = fault->disk.reads + 1;
    CHECK(!fault_seed->read(0, 8));
    CHECK(fault->mount.report().phase == Ext4MountPhase::failed);
    CHECK(fault_seed->release());
    auto drained = fault->mount.lifetime_snapshot(12); CHECK(drained && drained.value().total() == 0);

    const usize bitmap = r32(original, descriptor(original, 0) + 4) * 1024;
    CHECK(!(original[bitmap + (14 - 1) / 8] & (1u << ((14 - 1) % 8))));
    std::fill_n(original.begin() + inode_offset(original, 14), 256, 0);
    auto writable = std::make_unique<Mounted>(dirty_empty(original),
        Ext4MountedPolicy::ordered_data_dirty_noatime);
    testing::ext4_clock::set({{1700000200, 123456789}, {200, 1}, 1,
                              time::RealtimeOrigin::administrator_software});
    auto created = writable->root()->create_child("lifetime", {NodeType::regular, 0640, 71, 72});
    CHECK(created);
    auto checked_created = created.value()->query_metadata(); CHECK(checked_created);
    const u32 created_inode = checked_created.value().attributes.inode;
    auto created_snapshot = writable->mount.lifetime_snapshot(created_inode); CHECK(created_snapshot);
    CHECK(created_snapshot.value().key.generation ==
          r32(writable->disk.durable, inode_offset(writable->disk.durable, created_inode) + 100));
    CHECK(created.value()->retain());
    created_snapshot = writable->mount.lifetime_snapshot(created_inode); CHECK(created_snapshot);
    CHECK(created_snapshot.value().holds[static_cast<usize>(Ext4InodeHold::descriptor)] == 1);
    CHECK(created.value()->release());
    std::puts("PASS mounted inode lifetime: checked generation, PathReference, I/O hold, capacity and quarantine");
}
