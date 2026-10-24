#pragma once
#include "ok/core/types.hpp"
#include <string_view>
namespace ok::fs
{
inline constexpr std::string_view native_mount_snapshot_profile = "global-nine-slot-read-only-derived-generation-v1";
inline constexpr u32 native_mount_snapshot_version = 1;
inline constexpr u32 native_mount_snapshot_slots = 9;
inline constexpr u32 native_mount_snapshot_path_bytes = 256;
inline constexpr u32 native_mount_snapshot_wire_bytes = 288;
} // namespace ok::fs
