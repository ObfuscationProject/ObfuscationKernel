#pragma once
#include "ok/core/types.hpp"
#include <string_view>

namespace ok::fs
{
inline constexpr std::string_view boot_archive_directory_profile = "root-owned-ram-exact-sticky-sgid-v1";
inline constexpr u32 boot_archive_directory_mode_mask = 03777;
inline constexpr u32 boot_archive_regular_mode_mask = 0777;
} // namespace ok::fs
