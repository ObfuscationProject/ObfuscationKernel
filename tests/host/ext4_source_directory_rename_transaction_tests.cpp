#include "ext4_owned_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;

namespace
{
using Owner = Ext4OwnedTransaction<1, 8>;
usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "source directory transaction:%d: %s\n", __LINE__, #x); std::abort(); } } while (false)

Bytes dirty(Bytes image)
{
    w32(image, 1120, r32(image, 1120) | 4U);
    super_crc(image);
    return image;
}

u32 source_inode(const Bytes &image)
{
    Disk disk(image);
    Ext4ReplayPreview<4, 2> view(false, false, Ext4ReplayPolicy::directory_rename);
    CHECK(view.open(disk, {0, image.size() / 512}));
    const auto result = view.lookup(2, "inbox");
    CHECK(result && result.value() > 10);
    return result.value();
}

void verify_directory(const Bytes &image, u32 source)
{
    Disk disk(image);
    Ext4ReplayPreview<4, 2> view(false, false, Ext4ReplayPolicy::directory_rename);
    CHECK(view.open(disk, {0, image.size() / 512}));
    const auto found = view.lookup(2, "archived-inbox");
    CHECK(found && found.value() == source);
    CHECK(view.lookup(2, "inbox").status().code() == StatusCode::not_found);
    CHECK(view.lookup(source, ".").value() == source && view.lookup(source, "..").value() == 2);
    CHECK(view.lookup(source, "note.txt"));
    CHECK(!disk.writes && !disk.flushes);
}

void run(const Bytes &clean, const std::filesystem::path &out)
{
    const auto original = dirty(clean);
    const u32 source = source_inode(original);
    const usize slot = inode_offset(original, source);
    const u32 source_block = r32(original, slot + 60);
    const Ext4RenameUpdate update{2, source, 1700000222, 123456789, "inbox", "archived-inbox", true};
    const usize root_block = r32(original, inode_offset(original, 2) + 60) * 1024;
    for (u32 wrong_ancestor : {u32{11}, u32{15}})
    {
        auto corrupted = original;
        w32(corrupted, root_block + 12, wrong_ancestor);
        directory_crc(corrupted, 2, root_block);
        Disk rejected(corrupted); Owner::Registry local_registry;
        auto local_key = local_registry.register_device(rejected, 120 + wrong_ancestor); CHECK(local_key);
        auto local_owner = std::make_unique<Owner>(true);
        CHECK(local_owner->open(local_registry, local_key.value(), {0, corrupted.size() / 512}));
        const auto refusal = local_owner->prepare_rename(update);
        CHECK(!refusal && refusal.code() == StatusCode::io_error);
        CHECK(!rejected.writes && !rejected.flushes);
        CHECK(rejected.live == corrupted && rejected.durable == corrupted);
    }
    for (u32 drift : {u32{0}, u32{1}, u32{2}, u32{3}})
    {
        Disk drifted(original); Owner::Registry local_registry;
        auto local_key = local_registry.register_device(drifted, 141); CHECK(local_key);
        auto local_owner = std::make_unique<Owner>(true);
        CHECK(local_owner->open(local_registry, local_key.value(), {0, original.size() / 512}));
        CHECK(local_owner->prepare_rename(update));
        if (drift == 0)
        {
            w32(drifted.live, root_block + 12, 11);
            directory_crc(drifted.live, 2, root_block);
        }
        else if (drift < 3)
        {
            const u32 inode = drift == 1 ? 2 : source;
            const usize at = inode_offset(drifted.live, inode);
            w32(drifted.live, at + 12, r32(drifted.live, at + 12) ^ 1);
            inode_crc(drifted.live, inode, at);
        }
        else
        {
            const usize at = journal(drifted.live);
            big32(drifted.live, at + 24, be32(drifted.live, at + 24) + 1);
            big32(drifted.live, at + 252, 0);
            big32(drifted.live, at + 252, crc(~u32{0}, drifted.live, at, 1024));
        }
        CHECK(!local_owner->commit());
        CHECK(!drifted.writes && !drifted.flushes);
        CHECK(drifted.durable == original);
    }
    Disk disk(original); Owner::Registry registry;
    auto key = registry.register_device(disk, 81); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, original.size() / 512}));
    CHECK(owner->prepare_rename(update));
    CHECK(!disk.writes && !disk.flushes);
    CHECK(owner->commit());
    const auto logged = disk.durable;
    save(out / "published-directory-rename.img", logged);
    CHECK(owner->checkpoint());
    const auto final = disk.durable;
    CHECK(owner->close());
    save(out / "completed-directory-rename.img", final);
    CHECK(r16(final, slot + 26) == r16(original, slot + 26));
    CHECK(r16(final, inode_offset(final, 2) + 26) == r16(original, inode_offset(original, 2) + 26));
    CHECK(std::equal(original.begin() + source_block * 1024, original.begin() + (source_block + 1) * 1024,
                     final.begin() + source_block * 1024));
    verify_directory(final, source);
    Disk replay(logged);
    auto completion = std::make_unique<Ext4RecoveryCompletion<4, 2>>(false, true, Ext4ReplayPolicy::directory_rename);
    CHECK(completion->run(replay, {0, logged.size() / 512}));
    CHECK(replay.durable == final);
    for (usize prefix = 1; prefix <= 3; ++prefix)
    {
        Disk restart(original); Owner::Registry local_registry;
        auto local_key = local_registry.register_device(restart, u32(90 + prefix)); CHECK(local_key);
        auto local_owner = std::make_unique<Owner>(true);
        CHECK(local_owner->open(local_registry, local_key.value(), {0, original.size() / 512}));
        CHECK(local_owner->prepare_rename(update) && local_owner->commit());
        restart.fail_write_after = restart.writes + prefix;
        CHECK(!local_owner->checkpoint());
        const auto crashed = restart.durable;
        Disk recovered(crashed);
        auto recovery = std::make_unique<Ext4RecoveryCompletion<4, 2>>(false, true, Ext4ReplayPolicy::directory_rename);
        CHECK(recovery->run(recovered, {0, crashed.size() / 512}));
        CHECK(recovered.durable == final);
    }
    {
        Disk changed(original); Owner::Registry local_registry;
        auto local_key = local_registry.register_device(changed, 101); CHECK(local_key);
        auto local_owner = std::make_unique<Owner>(true);
        CHECK(local_owner->open(local_registry, local_key.value(), {0, original.size() / 512}));
        CHECK(local_owner->prepare_rename(update));
        changed.live[source_block * 1024 + 32] ^= 1;
        CHECK(!local_owner->commit());
        CHECK(!changed.writes && !changed.flushes);
    }
    {
        Disk mounted_disk(original); Ext4MountedMetadata<1>::Registry local_registry;
        auto local_key = local_registry.register_device(mounted_disk, 111); CHECK(local_key);
        auto mount = std::make_unique<Ext4MountedMetadata<1>>(Ext4MountedPolicy::ordered_data_dirty_noatime);
        CHECK(mount->mount(local_registry, local_key.value(), {0, original.size() / 512}));
        auto *root = mount->root(); CHECK(root);
        auto inbox = root->lookup_child("inbox"); CHECK(inbox);
        auto child = inbox.value()->lookup_child("note.txt"); CHECK(child);
        const auto child_inode = child.value()->query_metadata().value().attributes.inode;
        CHECK(root->rename("inbox", *root, "archived-inbox"));
        CHECK(root->lookup_child("inbox").status().code() == StatusCode::not_found);
        auto renamed = root->lookup_child("archived-inbox"); CHECK(renamed && renamed.value() == inbox.value());
        CHECK(renamed.value()->query_metadata().value().attributes.inode == source);
        CHECK(child.value()->query_metadata().value().attributes.inode == child_inode);
        CHECK(renamed.value()->lookup_child("note.txt").value() == child.value());
        const auto path = inbox.value()->directory_path(*root); CHECK(path && path.value().view() == "/archived-inbox");
        save(out / "mounted-directory-rename.img", mounted_disk.durable);
    }
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    ok::testing::ext4_clock::set({{1700000222, 123456789}, {222, 1}, 1,
        time::RealtimeOrigin::administrator_software});
    run(original, argv[2]);
    std::printf("EXT4_RENAME_PREIMAGE_REPAIR_TRANSACTION_MODEL_PASS checks=%zu\n", checks);
    return 0;
}
