#pragma once

// Generated from the exact reviewed local-name inventory; see docs/abi/linux-udp-name-profiles.json.
#include "ok/abi/linux_profile_data.hpp"

namespace ok::abi::linux_user
{
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> udp_getsockname_numbers{367U, 51U, 204U, 286U, 204U, 204U, 204U, 4172U, 5050U, 331U};
inline constexpr u32 udp_getsockname_subcall = 6U;
} // namespace ok::abi::linux_user
