#include "ext4_rename_relocation_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL owned rename:%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

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

void check_result(const Bytes &image)
{
    Disk disk(image);
    Ext4ReplayPreview<4, 2> view(false, false, Ext4ReplayPolicy::directory_rename);
    CHECK(view.open(disk, {0, image.size() / 512}));
    auto note = view.lookup(2, "saved-note.txt"); CHECK(note && note.value() == 12);
    CHECK(view.lookup(2, "notes.txt").status().code() == StatusCode::not_found);
    CHECK(!disk.writes && !disk.flushes);
}

void run(const Bytes &original, const char *published, const char *completed)
{
    auto dirty = dirty_empty(original);
    Disk disk(dirty); Owner::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, dirty.size() / 512}));
    CHECK(owner->prepare_rename(rename_update));
    CHECK(owner->report().phase == Ext4OwnedPhase::prepared);
    CHECK(owner->buffers().read_pins == 4 && owner->buffers().transaction_active);
    CHECK(!disk.writes && !disk.flushes && disk.live == dirty);
    CHECK(owner->commit());
    CHECK(owner->report().phase == Ext4OwnedPhase::durable);
    CHECK(disk.durable == disk.live);
    CHECK(disk.writes == 7 && disk.flushes == 4);
    const Bytes logged = disk.durable;
    save(published, logged);
    CHECK(owner->checkpoint());
    CHECK(owner->report().phase == Ext4OwnedPhase::checkpointed);
    CHECK(owner->report().completion.validated_actions == 3);
    CHECK(disk.writes == 11 && disk.flushes == 8);
    CHECK(disk.durable == disk.live);
    const Bytes final = disk.durable;
    save(completed, final);
    CHECK(owner->buffers().read_pins == 0 && !owner->buffers().transaction_active);
    CHECK(owner->close());
    check_result(final);
    Disk restart(logged);
    Ext4RecoveryCompletion<4, 2> recovery(false, true, Ext4ReplayPolicy::directory_rename);
    CHECK(recovery.run(restart, {0, logged.size() / 512}));
    CHECK(restart.durable == final && restart.live == final);
    Disk mounted_disk(logged);
    Ext4MountedMetadata<1>::Registry mounted_registry;
    auto mounted_key = mounted_registry.register_device(mounted_disk, 19); CHECK(mounted_key);
    auto mount = std::make_unique<Ext4MountedMetadata<1>>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(mount->mount(mounted_registry, mounted_key.value(), {0, logged.size() / 512}));
    auto *root = mount->root(); CHECK(root);
    auto mounted_note = root->lookup_child("saved-note.txt"); CHECK(mounted_note);
    CHECK(mounted_note.value()->query_metadata().value().attributes.inode == 12);
    CHECK(root->lookup_child("notes.txt").status().code() == StatusCode::not_found);
    CHECK(mounted_disk.durable == final && mounted_disk.live == final);
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes clean((std::istreambuf_iterator<char>(input)), {});
    const Bytes original = relocation_fixture::with_notes_source(clean);
    CHECK(original.size() == 32 * 1024 * 1024);
    const std::filesystem::path artifacts = argv[2];
    CHECK(std::filesystem::create_directories(artifacts));
    for (bool free_record : {false, true})
    {
        const auto prefix = artifacts / (free_record ? "free" : "slack");
        const auto published = prefix.string() + "-published.img", completed = prefix.string() + "-completed.img";
        run(free_record ? relocation_fixture::with_free_record(original) : original, published.c_str(), completed.c_str());
    }
    std::puts("PASS relocation owner: free/slack publication, checkpoint, byte-exact cold replay and mount reopening; scope=model");
}
