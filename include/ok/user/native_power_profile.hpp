#pragma once
#include "ok/core/types.hpp"
#include <string_view>
namespace ok::user
{
inline constexpr std::string_view native_power_profile = "original-init-scalar-prepare-queued-commit-v1";
inline constexpr u32 native_power_version = 1;
inline constexpr u32 native_power_token_limit = 0x3fffffff;
} // namespace ok::user
