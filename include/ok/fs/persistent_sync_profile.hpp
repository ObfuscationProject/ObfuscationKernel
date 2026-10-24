#pragma once

#include "ok/core/types.hpp"
#include <string_view>

namespace ok::fs
{
inline constexpr std::string_view native_persistent_sync_profile = "retained-linked-mounted-ordered-journal-device-flush-v1";
inline constexpr u32 native_persistent_sync_version = 1;
} // namespace ok::fs
