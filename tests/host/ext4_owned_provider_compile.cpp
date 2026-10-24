#include "ok/fs/ext4_fs.hpp"
#include <type_traits>

static_assert(std::is_final_v<ok::fs::Ext4Node>);
static_assert(std::is_base_of_v<ok::fs::Node, ok::fs::Ext4Node>);
static_assert(!std::is_copy_constructible_v<ok::fs::Ext4Node>);
static_assert(!std::is_copy_assignable_v<ok::fs::Ext4Node>);
static_assert(!std::is_move_constructible_v<ok::fs::Ext4Node>);
static_assert(!std::is_move_assignable_v<ok::fs::Ext4Node>);
// Freestanding API probe: instantiate the provider's retained and borrowed
// public entry points with every architecture-selected memory lock profile.
ok::Status legacy_owned_provider_probe(ok::fs::Ext4Node &parent, ok::fs::Ext4Node &child)
{
    auto found = parent.lookup_child_retained("file");
    if (!found) return found.status();
    auto status = found.value()->release();
    if (!status) return status;
    auto made = parent.create_child_retained("new", {ok::fs::NodeType::regular, 0644, 0, 0});
    if (!made) return made.status();
    status = made.value()->release();
    if (!status) return status;
    return child.expose_borrowed_identity();
}
