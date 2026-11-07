#pragma once
#include "ok/core/types.hpp"
#include <string_view>
namespace ok::user
{
inline constexpr std::string_view native_tty_profile = "bootcpu-ntty-template-console-pts8-devfs-ptmx-tty-console-v1";
inline constexpr u32 native_tty_version = 1;
inline constexpr u32 native_tty_module_abi = 79;
inline constexpr u32 native_tty_serialized_boot_cpu = 1;
inline constexpr u32 native_tty_terminal_capacity = 9;
inline constexpr u32 native_tty_pts_capacity = 8;
inline constexpr u32 native_tty_pts_major = 136;
inline constexpr u32 native_tty_read_capacity = 4096;
inline constexpr u32 native_tty_echo_capacity = 512;
inline constexpr u32 native_tty_master_queue_capacity = 4096;
inline constexpr u32 native_tty_signal_outbox_capacity = 8;
inline constexpr u32 native_tty_console_write_limit = 65536;
inline constexpr u32 native_tty_native_termios_bytes = 36;
inline constexpr u32 native_tty_native_nccs = 19;
inline constexpr u32 native_tty_winsize_bytes = 8;
inline constexpr u32 native_tty_linux_codec_revision = 3;
inline constexpr u32 native_tty_canonical = 1;
inline constexpr u32 native_tty_echo = 1;
inline constexpr u32 native_tty_isig = 1;
inline constexpr u32 native_tty_opost = 1;
inline constexpr u32 native_tty_iutf8 = 1;
inline constexpr u32 native_tty_vmin_vtime = 1;
inline constexpr u32 native_tty_pty_vtime = 1;
inline constexpr u32 native_tty_pty_ixon = 1;
inline constexpr u32 native_tty_console_compat_default = 1;
inline constexpr u32 native_tty_console_vtime = 0;
inline constexpr u32 native_tty_console_vhangup = 0;
inline constexpr u32 native_tty_console_ixon = 0;
inline constexpr u32 native_tty_parity = 0;
inline constexpr u32 native_tty_ixany_ixoff = 0;
inline constexpr u32 native_tty_imaxbel = 0;
inline constexpr u32 native_tty_case_mapping = 0;
inline constexpr u32 native_tty_vdiscard = 0;
inline constexpr u32 native_tty_baud_reprogramming = 0;
inline constexpr u32 native_tty_dev_tty = 1;
inline constexpr u32 native_tty_dev_console = 1;
inline constexpr u32 native_tty_dev_ptmx = 1;
inline constexpr u32 native_tty_pts_mode = 0620;
inline constexpr u32 native_tty_pts_owner_opener = 1;
inline constexpr u32 native_tty_grantpt_noop = 1;
inline constexpr u32 native_tty_locked_listed = 1;
inline constexpr u32 native_tty_separate_devpts_mount = 0;
inline constexpr u32 native_tty_stat_rdev_linux = 1;
inline constexpr u32 native_tty_stat_rdev_native = 0;
inline constexpr u32 native_tty_tiocgptpeer = 0;
inline constexpr u32 native_tty_packet_mode = 0;
inline constexpr u32 native_tty_tiocsti = 0;
inline constexpr u32 native_tty_termios2 = 0;
inline constexpr u32 native_tty_smp = 0;
} // namespace ok::user
