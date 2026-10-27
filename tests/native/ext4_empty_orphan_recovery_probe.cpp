#include <ok/fs/ext4/mount.hpp>
#include <type_traits>
using Recovery = ok::fs::Ext4EmptyOrphanRecoveryOwner<1>;
using Workspace = ok::fs::Ext4EmptyOrphanRecoveryWorkspace;
static_assert(!std::is_copy_constructible_v<Recovery> && !std::is_move_constructible_v<Recovery>);
static_assert(!std::is_copy_constructible_v<Workspace> && !std::is_move_constructible_v<Workspace>);
extern "C" ok::Status ext4_empty_orphan_probe(Recovery &owner, Recovery::Registry &registry,
    Recovery::Registry::Registration registration, ok::block::SessionBounds bounds, Workspace &workspace)
{
    if (auto status = owner.open(registry, registration, bounds, workspace); !status) return status;
    if (auto status = owner.recover(); !status) return status;
    const auto report = owner.report(); const auto buffers = owner.buffers();
    if (!report.complete || buffers.state != ok::fs::MetadataOwnerState::ready)
        return ok::Status::fault("empty orphan probe is incomplete");
    return owner.close();
}
