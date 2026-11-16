#pragma once
#include "ok/core/types.hpp"
#include <string_view>
namespace ok::user
{
inline constexpr std::string_view native_sessions_profile = "bootcpu-serialized-process-group-session-console-pty-jobctl-lifecycle-v4";
inline constexpr u32 native_sessions_version = 4;
inline constexpr u32 native_sessions_module_abi = 81;
inline constexpr u32 native_sessions_pid_bits = 32;
inline constexpr u32 native_sessions_process_capacity = 64;
inline constexpr u32 native_sessions_fork_inherits = 1;
inline constexpr u32 native_sessions_exec_preserves = 1;
inline constexpr u32 native_sessions_reparent_preserves = 1;
inline constexpr u32 native_sessions_controlling_tty = 1;
inline constexpr u32 native_sessions_ctty_derived = 1;
inline constexpr u32 native_sessions_orphaned_group_hup = 1;
inline constexpr u32 native_sessions_pty = 1;
inline constexpr u32 native_sessions_job_control = 1;
} // namespace ok::user
