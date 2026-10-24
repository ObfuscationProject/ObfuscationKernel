#pragma once

// Generated from the retained connected UDP inventory; see docs/abi/linux-udp-connected-profiles.json.
#include "ok/abi/linux_profile_data.hpp"

namespace ok::abi::linux_user
{
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> udp_connect_numbers{362U, 42U, 203U, 283U, 203U, 203U, 203U, 4170U, 5041U, 328U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> udp_getpeername_numbers{368U, 52U, 205U, 287U, 205U, 205U, 205U, 4171U, 5051U, 332U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> udp_enotconn_numbers{107U, 107U, 107U, 107U, 107U, 107U, 107U, 134U, 134U, 107U};
inline constexpr u32 udp_connect_subcall = 3U;
inline constexpr u32 udp_getpeername_subcall = 7U;
} // namespace ok::abi::linux_user
