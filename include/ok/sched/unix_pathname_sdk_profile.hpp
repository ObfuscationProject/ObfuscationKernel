#pragma once
#include "ok/core/types.hpp"

namespace ok::sched
{
inline constexpr std::string_view native_unix_pathname_profile = "bootcpu-ram-pathname-stream-owned-queue-direct-v1";
inline constexpr std::string_view linux_unix_pathname_profile = "bootcpu-ram-pathname-stream-owned-queue-direct-v1";
inline constexpr u32 unix_pathname_module_abi = 75;
inline constexpr usize unix_pathname_endpoint_capacity = 32;
inline constexpr usize unix_pathname_backlog_capacity = 4;
inline constexpr usize unix_pathname_address_capacity = 110;
inline constexpr usize unix_pathname_text_capacity = 107;
inline constexpr usize unix_pathname_peer_credentials_bytes = 12;
}
