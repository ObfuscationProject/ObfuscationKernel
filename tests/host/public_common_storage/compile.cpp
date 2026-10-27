#include <ok/fs/ext4/mount.hpp>
#include <type_traits>
using Mount = ok::fs::Ext4MountedMetadata<2>;
static_assert(!std::is_copy_constructible_v<Mount::CommonStorage>);
static_assert(!std::is_move_constructible_v<Mount::CommonStorage>);
void construct_public_common(Mount::CommonStorage *storage, Mount *mount)
{
    std::construct_at(storage);
    std::construct_at(mount, *storage);
}
ok::Status public_common_boundary(Mount &mount, Mount::Registry &registry,
    Mount::Registry::Registration key, ok::block::SessionBounds bounds,
    ok::fs::Node &node, std::span<const std::byte> bytes)
{
    if (auto status = mount.mount(registry, key, bounds); !status) return status;
    auto root = mount.retain_root(); if (!root) return root.status();
    if (auto status = node.truncate(0); !status) return status;
    if (auto status = node.write(7, bytes); !status) return status;
    auto read = node.read(0, 73); if (!read) return read.status();
    if (auto status = root.value().release(); !status) return status;
    return mount.close();
}
