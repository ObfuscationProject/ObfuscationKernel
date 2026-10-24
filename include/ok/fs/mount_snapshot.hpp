#pragma once

#include "ok/core/types.hpp"
#include <array>

namespace ok::fs
{
inline constexpr u32 mount_snapshot_capacity = 9;
inline constexpr usize mount_snapshot_path_capacity = 256;

enum class MountSnapshotState : u32
{
    unused = 0,
    underlying_ram = 1,
    borrowed_provider = 2,
    owned_provider = 3,
    owned_ram = 4,
    detached = 5,
};

// An owned observation of one registered slot, without authority or node holds.
// Generation is local to one VFS lifetime and changes only with mount topology.
struct MountSnapshot
{
    u32 generation{}, slot{};
    MountSnapshotState state{MountSnapshotState::unused};
    u32 parent_id{}, path_length{};
    std::array<char, mount_snapshot_path_capacity> path{};
};
} // namespace ok::fs
