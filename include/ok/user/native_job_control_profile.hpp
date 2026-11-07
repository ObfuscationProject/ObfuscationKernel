#pragma once
#include "ok/core/types.hpp"
#include <string_view>
namespace ok::user
{
inline constexpr std::string_view native_job_control_profile = "bootcpu-overlay-stop-cont-wuntraced-wcontinued-pgrp-wait-nocldstop-owed-kernel-v1";
inline constexpr u32 native_job_control_version = 1;
inline constexpr u32 native_job_control_module_abi = 79;
inline constexpr u32 native_job_control_serialized_boot_cpu = 1;
inline constexpr u32 native_job_control_process_capacity = 64;
inline constexpr u32 native_job_control_signal_max = 64;
inline constexpr u32 native_job_control_owed_kernel_records = 64;
inline constexpr u32 native_job_control_sigaction_bytes = 24;
inline constexpr u32 native_job_control_cld_stopped = 5;
inline constexpr u32 native_job_control_cld_continued = 6;
inline constexpr u32 native_job_control_stop_overlay = 1;
inline constexpr u32 native_job_control_wait_untraced = 1;
inline constexpr u32 native_job_control_wait_continued = 1;
inline constexpr u32 native_job_control_group_wait = 1;
inline constexpr u32 native_job_control_waitid_pgid = 1;
inline constexpr u32 native_job_control_nowait = 1;
inline constexpr u32 native_job_control_waitid_rv32_only = 1;
inline constexpr u32 native_job_control_nocldstop = 1;
inline constexpr u32 native_job_control_orphaned_stop_discard = 1;
inline constexpr u32 native_job_control_orphan_hangup = 1;
inline constexpr u32 native_job_control_same_session_continue = 1;
inline constexpr u32 native_job_control_native_dfl_ign = 1;
inline constexpr u32 native_job_control_init_protection = 1;
inline constexpr u32 native_job_control_owed_kernel_signals = 1;
inline constexpr u32 native_job_control_native_handlers = 0;
inline constexpr u32 native_job_control_native_masks = 0;
inline constexpr u32 native_job_control_native_child_sigchld = 0;
inline constexpr u32 native_job_control_broadcast = 0;
inline constexpr u32 native_job_control_ptrace = 0;
inline constexpr u32 native_job_control_core_dump = 0;
inline constexpr u32 native_job_control_smp = 0;
} // namespace ok::user
