#pragma once
#include "ok/core/types.hpp"
#include <string_view>
namespace ok::user
{
inline constexpr std::string_view native_group_signal_profile = "bootcpu-signed32-current-negative-pgid-probe-all-signals-stop-continue-orphan-hupcont-same-session-cont-partial-v3";
inline constexpr u32 native_group_signal_version = 3;
inline constexpr u32 native_group_signal_module_abi = 81;
inline constexpr u32 native_group_signal_pid_bits = 32;
inline constexpr u32 native_group_signal_process_capacity = 64;
inline constexpr u32 native_group_signal_current_group = 1;
inline constexpr u32 native_group_signal_negative_group = 1;
inline constexpr u32 native_group_signal_probe = 1;
inline constexpr u32 native_group_signal_term = 15;
inline constexpr u32 native_group_signal_kill = 9;
inline constexpr u32 native_group_signal_deferred_fatal = 1;
inline constexpr u32 native_group_signal_partial_success = 1;
inline constexpr u32 native_group_signal_default_actions = 1;
inline constexpr u32 native_group_signal_same_session_continue = 1;
inline constexpr u32 native_group_signal_orphan_hangup = 1;
inline constexpr u32 native_group_signal_broadcast = 0;
inline constexpr u32 native_group_signal_stop_continue = 1;
inline constexpr u32 native_group_signal_controlling_tty = 1;
inline constexpr u32 native_group_signal_job_control = 1;
inline constexpr u32 native_group_signal_smp = 0;
} // namespace ok::user
