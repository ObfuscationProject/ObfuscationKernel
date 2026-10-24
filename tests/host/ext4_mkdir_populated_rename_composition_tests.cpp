// Hosted composition model; no hardware or user-space execution claim.
#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace owned_fixture;
using Mount = Ext4MountedMetadata<1>;
namespace clock_model = ok::testing::ext4_clock;
namespace
{
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "mkdir/rename composition:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)

Bytes old_image(Bytes bytes)
{
    const usize bitmap = r32(bytes, descriptor(bytes, 0) + 4) * 1024;
    for (u32 number : {14U, 15U})
    {
        CHECK(!(bytes[bitmap + (number - 1) / 8] & (1U << ((number - 1) % 8))));
        std::fill_n(bytes.begin() + inode_offset(bytes, number), 256, 0);
    }
    CHECK(std::any_of(bytes.begin() + 535 * 1024, bytes.begin() + 536 * 1024,
        [](u8 byte) { return byte != 0; }));
    w32(bytes, 1120, r32(bytes, 1120) | 4U); super_crc(bytes);
    return bytes;
}

void run(const Bytes &original, const char *output)
{
    Disk disk(original); Mount::Registry registry;
    const auto key = registry.register_device(disk, 241); CHECK(key);
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(mount->mount(registry, key.value(), {0, original.size() / 512}));
    clock_model::set({{1700000444, 123456789}, {444, 1}, 1, time::RealtimeOrigin::administrator_software});
    auto *root = mount->root(); CHECK(root);
    const auto before = root->query_metadata(); CHECK(before);
    auto directory = root->create_child_retained("drafts", {NodeType::directory, 0755, 0, 0}); CHECK(directory);
    const auto created = directory.value()->query_metadata(); CHECK(created);
    const auto directory_inode = created.value().attributes.inode;
    CHECK(created.value().type == NodeType::directory && created.value().size == 1024 &&
        created.value().blocks == 2 && created.value().link_count == 2);
    CHECK(root->query_metadata().value().link_count == before.value().link_count + 1);
    CHECK(root->rename("drafts", *root, "documents"));
    CHECK(root->lookup_child("drafts").status().code() == StatusCode::not_found);
    CHECK(root->lookup_child("documents").value() == directory.value());
    auto child = directory.value()->create_child_retained("note.txt", {NodeType::regular, 0644, 0, 0}); CHECK(child);
    const auto child_metadata = child.value()->query_metadata(); CHECK(child_metadata);
    const auto child_inode = child_metadata.value().attributes.inode;
    CHECK(child_metadata.value().type == NodeType::regular && child_metadata.value().size == 0);
    const auto before_rename = disk.durable;
    const usize source_block = usize(r32(before_rename, inode_offset(before_rename, u32(directory_inode)) + 60)) * 1024;
    CHECK(root->rename("documents", *root, "archived-documents"));
    CHECK(root->lookup_child("documents").status().code() == StatusCode::not_found);
    CHECK(root->lookup_child("archived-documents").value() == directory.value());
    CHECK(directory.value()->query_metadata().value().attributes.inode == directory_inode);
    CHECK(child.value()->query_metadata().value().attributes.inode == child_inode);
    CHECK(directory.value()->lookup_child("note.txt").value() == child.value());
    const auto path = directory.value()->directory_path(*root); CHECK(path && path.value().view() == "/archived-documents");
    CHECK(std::equal(before_rename.begin() + source_block, before_rename.begin() + source_block + 1024,
        disk.durable.begin() + source_block));
    CHECK(root->query_metadata().value().link_count == before.value().link_count + 1);
    CHECK(child.value()->release() && directory.value()->release());
    CHECK(root->sync()); save(output, disk.durable);
    Disk cold(disk.durable); Mount::Registry cold_registry;
    const auto cold_key = cold_registry.register_device(cold, 242); CHECK(cold_key);
    auto reopened = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(reopened->mount(cold_registry, cold_key.value(), {0, cold.live.size() / 512}));
    auto found = reopened->root()->lookup_child("archived-documents"); CHECK(found);
    CHECK(found.value()->query_metadata().value().attributes.inode == directory_inode);
    auto note = found.value()->lookup_child("note.txt"); CHECK(note);
    CHECK(note.value()->query_metadata().value().attributes.inode == child_inode);
    CHECK(found.value()->lookup_child(".").value() == found.value());
    CHECK(found.value()->lookup_child("..").value() == reopened->root());
    CHECK(reopened->root()->lookup_child("drafts").status().code() == StatusCode::not_found);
    CHECK(reopened->root()->lookup_child("documents").status().code() == StatusCode::not_found);
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes image((std::istreambuf_iterator<char>(input)), {});
    CHECK(image.size() == 32 * 1024 * 1024);
    run(old_image(image), argv[2]);
    std::printf("EXT4_MKDIR_POPULATED_RENAME_COMPOSITION_MODEL_PASS checks=%zu\n", checks);
}
