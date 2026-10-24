#pragma once

#include "ok/fs/mount_snapshot.hpp"
#include "ok/fs/mount_snapshot_profile.hpp"
#include "ok/syscall/linux.hpp"
#include "ok/uapi/syscall.h"
#include <array>
#include <bit>
#include <cstddef>
#include <span>
#include <type_traits>

namespace ok::user::native_mount_info
{
static_assert(sizeof(ok_mount_info) == 288 && sizeof(ok_mount_info) <= 1024);
static_assert(offsetof(ok_mount_info, version) == 0 && offsetof(ok_mount_info, size) == 4);
static_assert(offsetof(ok_mount_info, generation) == 8 && offsetof(ok_mount_info, slot) == 12);
static_assert(offsetof(ok_mount_info, capacity) == 16 && offsetof(ok_mount_info, state) == 20);
static_assert(offsetof(ok_mount_info, parent_id) == 24 && offsetof(ok_mount_info, path_length) == 28);
static_assert(offsetof(ok_mount_info, path) == 32);
static_assert(OK_NATIVE_MOUNT_INFO_VERSION == fs::native_mount_snapshot_version);
static_assert(OK_MOUNT_INFO_CAPACITY == fs::mount_snapshot_capacity &&
              OK_MOUNT_INFO_PATH_CAPACITY == fs::mount_snapshot_path_capacity);
static_assert(static_cast<u32>(fs::MountSnapshotState::unused) == OK_MOUNT_STATE_UNUSED &&
              static_cast<u32>(fs::MountSnapshotState::underlying_ram) == OK_MOUNT_STATE_UNDERLYING_RAM &&
              static_cast<u32>(fs::MountSnapshotState::borrowed_provider) == OK_MOUNT_STATE_BORROWED &&
              static_cast<u32>(fs::MountSnapshotState::owned_provider) == OK_MOUNT_STATE_OWNED_PROVIDER &&
              static_cast<u32>(fs::MountSnapshotState::owned_ram) == OK_MOUNT_STATE_OWNED_RAM &&
              static_cast<u32>(fs::MountSnapshotState::detached) == OK_MOUNT_STATE_DETACHED);

// Serialize fields rather than a temporary C++ object's representation. Every
// unused byte starts at zero, including the path terminator and full suffix.
[[nodiscard]] inline std::array<u8, sizeof(ok_mount_info)> encode(const fs::MountSnapshot &snapshot)
{
    std::array<u8, sizeof(ok_mount_info)> output{};
    const u32 fields[]{OK_NATIVE_MOUNT_INFO_VERSION, sizeof(ok_mount_info), snapshot.generation,
                       snapshot.slot, OK_MOUNT_INFO_CAPACITY, static_cast<u32>(snapshot.state),
                       snapshot.parent_id, snapshot.path_length};
    for (usize field = 0; field < std::size(fields); ++field)
    {
        const auto bytes = std::bit_cast<std::array<u8, sizeof(u32)>>(fields[field]);
        for (usize index = 0; index < bytes.size(); ++index)
            output[field * sizeof(u32) + index] = bytes[index];
    }
    for (usize index = 0; index < snapshot.path_length; ++index)
        output[offsetof(ok_mount_info, path) + index] = static_cast<u8>(snapshot.path[index]);
    return output;
}

// The runtime authenticates the running caller. Only these injected seams touch
// VFS state or checked user memory. No caller pointer or VFS reference escapes.
// A failing copy may have stored a prefix; copy-fault atomicity is not promised.
template <typename Word, typename Snapshot, typename Range, typename CopyOut>
i64 dispatch(bool native_personality, const std::array<Word, 6> &arguments,
             Snapshot &&snapshot, Range &&range, CopyOut &&copy_out)
{
    static_assert(std::is_unsigned_v<Word> && sizeof(Word) >= sizeof(u32));
    if (!native_personality) return -38;
    if (arguments[0] >= OK_MOUNT_INFO_CAPACITY || arguments[2] != sizeof(ok_mount_info) ||
        arguments[3] || arguments[4] || arguments[5]) return -22;
    if (!range(arguments[1], sizeof(ok_mount_info))) return -14;
    fs::MountSnapshot captured{};
    if (const auto status = snapshot(static_cast<u32>(arguments[0]), captured); !status)
        return -syscall::ErrnoMapper::errno_for(status);
    if (captured.slot != arguments[0] || captured.parent_id >= OK_MOUNT_INFO_CAPACITY ||
        !captured.generation || captured.generation > 17 ||
        static_cast<u32>(captured.state) > OK_MOUNT_STATE_DETACHED ||
        captured.path_length >= OK_MOUNT_INFO_PATH_CAPACITY) return -5;
    const auto bytes = encode(captured);
    return copy_out(arguments[1], std::span<const u8>{bytes}) ? 0 : -14;
}
} // namespace ok::user::native_mount_info
