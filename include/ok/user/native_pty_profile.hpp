#pragma once
#include "ok/core/types.hpp"
#include <string_view>
namespace ok::user
{
inline constexpr std::string_view native_pty_profile = "bootcpu-native-raw-pair-ofd-waits-drain-eof-eio-v1";
inline constexpr u32 native_pty_version = 1;
inline constexpr u32 native_pty_module_abi = 75;
inline constexpr u32 native_pty_pair_capacity = 8;
inline constexpr u32 native_pty_queue_capacity = 4096;
inline constexpr u32 native_pty_generation_bits = 64;
inline constexpr u32 native_pty_descriptor_bytes = 4;
inline constexpr u32 native_pty_pair_output_bytes = 8;
inline constexpr u32 native_pty_serialized_boot_cpu = 1;
inline constexpr u32 native_pty_raw = 1;
inline constexpr u32 native_pty_retained_ofd_waits = 1;
inline constexpr u32 native_pty_unconditional_hangup = 1;
inline constexpr u32 native_pty_slave_master_closed_poll_error = 1;
inline constexpr u32 native_pty_master_slave_closed_poll_error = 0;
inline constexpr u32 native_pty_scm_rights = 0;
inline constexpr u32 native_pty_linux_creation = 0;
inline constexpr u32 native_pty_slave_reopen = 0;
inline constexpr u32 native_pty_termios = 0;
inline constexpr u32 native_pty_controlling_terminal = 0;
inline constexpr u32 native_pty_terminal_signals = 0;
inline constexpr u32 native_pty_job_control = 0;
inline constexpr u32 native_pty_devpts = 0;
inline constexpr u32 native_pty_smp = 0;
} // namespace ok::user
