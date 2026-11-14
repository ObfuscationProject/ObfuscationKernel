#pragma once
#include "ok/core/types.hpp"
#include <string_view>
namespace ok::user
{
inline constexpr std::string_view native_console_control_profile = "bootcpu-lease-epoch-explicit-console-pty-session-foreground-sigttin-sigttou-pid32-ioctl-v2";
inline constexpr u32 native_console_control_version = 2;
inline constexpr u32 native_console_control_module_abi = 80;
inline constexpr u32 native_console_control_serialized_boot_cpu = 1;
inline constexpr u32 native_console_control_lease_epoch = 1;
inline constexpr u32 native_console_control_explicit_acquire = 1;
inline constexpr u32 native_console_control_original_lease_session_leader = 1;
inline constexpr u32 native_console_control_foreground_read = 1;
inline constexpr u32 native_console_control_background_eio = 0;
inline constexpr u32 native_console_control_background_sigttin = 1;
inline constexpr u32 native_console_control_background_sigttou = 1;
inline constexpr u32 native_console_control_same_session_background_control = 1;
inline constexpr u32 native_console_control_zombie_group_until_reap = 1;
inline constexpr u32 native_console_control_pid_bytes = 4;
inline constexpr u32 native_console_control_native_uniform_requests = 1;
inline constexpr u32 native_console_control_linux_generated_requests = 1;
inline constexpr u32 native_console_control_auto_open_acquire = 1;
inline constexpr u32 native_console_control_console_auto_open_acquire = 0;
inline constexpr u32 native_console_control_dev_tty = 1;
inline constexpr u32 native_console_control_noctty_open = 1;
inline constexpr u32 native_console_control_force_steal = 0;
inline constexpr u32 native_console_control_empty_foreground_replaced = 0;
inline constexpr u32 native_console_control_pty = 1;
inline constexpr u32 native_console_control_termios = 1;
inline constexpr u32 native_console_control_stop_continue = 1;
inline constexpr u32 native_console_control_job_control = 1;
inline constexpr u32 native_console_control_smp = 0;
} // namespace ok::user
