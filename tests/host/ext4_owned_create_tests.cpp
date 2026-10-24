#include "ext4_owned_support.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace owned_fixture;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL owned create:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

namespace
{
using Owner = Ext4OwnedTransaction<1, 8>;
constexpr Ext4CreateUpdate create_update{2, 1700000111, 123456789, "empty-note",
    {NodeType::regular, 0600, 1000, 1001}};
usize restarts{};

Bytes dirty_empty(Bytes image)
{
    w32(image, 1120, r32(image, 1120) | 4U); super_crc(image); return image;
}

void check_result(Disk &disk, bool created)
{
    auto view = std::make_unique<Ext4ReplayPreview<6, 2>>(false, false, Ext4ReplayPolicy::directory_create);
    CHECK(view->open(disk, {0, disk.live.size() / 512}));
    const auto found = view->lookup(2, create_update.name);
    if (!created) { CHECK(!found && found.status().code() == StatusCode::not_found); return; }
    CHECK(found && found.value() >= 13);
    auto inode = view->inode(found.value()); CHECK(inode);
    CHECK(inode.value().mode == 0100600 && inode.value().uid == 1000 && inode.value().gid == 1001);
    CHECK(inode.value().size == 0 && inode.value().links == 1);
    std::array<u8, 4> bytes{1, 2, 3, 4};
    const auto read = view->read(found.value(), 0, bytes);
    CHECK(read && read.value() == 0 && bytes == (std::array<u8, 4>{1, 2, 3, 4}));
    CHECK(view->lookup(2, "seed").value() == 12);
}

void restart(const Bytes &durable, bool created)
{
    Disk disk(durable);
    Ext4MountedMetadata<1>::Registry registry;
    const auto key = registry.register_device(disk, 19); CHECK(key);
    auto mount = std::make_unique<Ext4MountedMetadata<1>>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    const auto status = mount->mount(registry, key.value(), {0, durable.size() / 512});
    if (!status) std::fprintf(stderr, "restart failed: %s\n", status.message().data());
    CHECK(status);
    auto *root = mount->root(); CHECK(root);
    const auto found = root->lookup_child(create_update.name);
    CHECK(created ? bool(found) : (!found && found.status().code() == StatusCode::not_found));
    if (created)
    {
        const auto metadata = found.value()->query_metadata(); CHECK(metadata);
        CHECK(metadata.value().mode == 0100600 && metadata.value().uid == 1000 && metadata.value().gid == 1001);
        CHECK(metadata.value().size == 0 && metadata.value().blocks == 0 && metadata.value().link_count == 1);
        CHECK(metadata.value().attributes.inode >= 13);
    }
    CHECK(disk.live == disk.durable);
    ++restarts;
}

void rejected(const Bytes &dirty, Ext4CreateUpdate update, StatusCode expected)
{
    Disk disk(dirty); Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    const auto status = owner->prepare_create(update);
    CHECK(status.code() == expected);
    CHECK(owner->report().phase == Ext4OwnedPhase::rejected);
    CHECK(!owner->buffers().read_pins && !owner->buffers().transaction_active);
    CHECK(!disk.writes && !disk.flushes && disk.live == dirty && disk.durable == dirty);
    CHECK(owner->close());
}

struct Counts { usize publication_writes{}, publication_flushes{}, checkpoint_writes{}, checkpoint_flushes{}; };
Counts successful(const Bytes &dirty, const std::filesystem::path &out)
{
    Disk disk(dirty); disk.origin = 37; Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {disk.origin, dirty.size() / 512}));
    auto status = owner->prepare_create(create_update);
    if (!status) std::fprintf(stderr, "prepare failed: %s\n", status.message().data());
    CHECK(status);
    CHECK(owner->report().phase == Ext4OwnedPhase::prepared && owner->created_inode() >= 13);
    CHECK(owner->buffers().read_pins == 7 && owner->buffers().transaction_active);
    CHECK(!disk.writes && !disk.flushes && disk.live == dirty && disk.durable == dirty);
    std::array<u8, 1024> unauthorized_data{};
    unauthorized_data.fill(0xa5);
    const Ext4InodeMetadataUpdate intrusive{12, 1000, 1001, 1700000111, 123456789,
        0600, true, 4096, 1700000111, 123456789};
    CHECK(owner->prepare_ordered_data(intrusive, 123, unauthorized_data).code() == StatusCode::busy);
    CHECK(owner->report().phase == Ext4OwnedPhase::prepared && owner->buffers().read_pins == 7);
    CHECK(owner->commit());
    CHECK(owner->report().phase == Ext4OwnedPhase::durable && disk.live == disk.durable);
    Counts counts{disk.writes, disk.flushes};
    save(out / "published.img", disk.durable);
    CHECK(owner->checkpoint());
    counts.checkpoint_writes = disk.writes - counts.publication_writes;
    counts.checkpoint_flushes = disk.flushes - counts.publication_flushes;
    CHECK(owner->report().phase == Ext4OwnedPhase::checkpointed && disk.live == disk.durable);
    CHECK(owner->report().completion.validated_actions == 6);
    CHECK(!owner->buffers().read_pins && !owner->buffers().transaction_active);
    CHECK(owner->close());
    save(out / "completed.img", disk.durable);
    Disk verify(disk.durable); check_result(verify, true);
    CHECK(!verify.writes && !verify.flushes);
    return counts;
}

void fault(const Bytes &dirty, bool checkpoint, usize write, usize flush, bool after)
{
    Disk disk(dirty); Owner::Registry registry;
    const auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    CHECK(owner->prepare_create(create_update));
    if (checkpoint) CHECK(owner->commit());
    const usize initial_writes = disk.writes, initial_flushes = disk.flushes;
    if (write) (after ? disk.fail_write_after : disk.fail_write) = initial_writes + write;
    if (flush) (after ? disk.fail_flush_after : disk.fail_flush) = initial_flushes + flush;
    CHECK(!(checkpoint ? owner->checkpoint() : owner->commit()));
    CHECK(owner->report().phase == Ext4OwnedPhase::failed && owner->buffers().state == MetadataOwnerState::failed);
    CHECK(owner->prepare_create(create_update).code() == StatusCode::denied);
    const bool created = checkpoint || (after && ((write && write == 10) || (flush && flush == 4)));
    disk.crash(); restart(disk.durable, created);
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes original((std::istreambuf_iterator<char>(input)), {});
    if (argc == 4 && std::string_view(argv[3]) == "revoke") optional_revoke_fixture(original);
    CHECK(original.size() == 32 * 1024 * 1024);
    const Bytes dirty = dirty_empty(original);
    std::filesystem::create_directory(argv[2]);
    const auto counts = successful(dirty, argv[2]);
    CHECK(counts.publication_writes == 10 && counts.publication_flushes == 4);
    CHECK(counts.checkpoint_writes == 7);
    auto update = create_update; update.name = "seed"; rejected(dirty, update, StatusCode::already_exists);
    update = create_update; update.nanoseconds = 1000000000; rejected(dirty, update, StatusCode::invalid_argument);
    update = create_update; update.attributes.type = NodeType::symlink; rejected(dirty, update, StatusCode::unsupported);
    {
        Disk disk(dirty); Owner::Registry registry;
        const auto key = registry.register_device(disk, 17); CHECK(key);
        auto owner = std::make_unique<Owner>(true);
        CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
        update = create_update; update.attributes.type = NodeType::directory;
        CHECK(owner->prepare_create(update).code() == StatusCode::unsupported);
        CHECK(owner->report().phase == Ext4OwnedPhase::open && !disk.writes && !disk.flushes);
        CHECK(owner->close());
    }
    for (const bool after : {false, true})
    {
        for (usize i = 1; i <= counts.publication_writes; ++i) fault(dirty, false, i, 0, after);
        for (usize i = 1; i <= counts.publication_flushes; ++i) fault(dirty, false, 0, i, after);
        for (usize i = 1; i <= counts.checkpoint_writes; ++i) fault(dirty, true, i, 0, after);
        for (usize i = 1; i <= counts.checkpoint_flushes; ++i) fault(dirty, true, 0, i, after);
    }
    std::printf("PASS hosted create owner restarts=%zu publication_writes=%zu publication_flushes=%zu checkpoint_writes=%zu checkpoint_flushes=%zu\n",
        restarts, counts.publication_writes, counts.publication_flushes, counts.checkpoint_writes, counts.checkpoint_flushes);
}
