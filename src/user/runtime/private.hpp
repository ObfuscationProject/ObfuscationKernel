#pragma once
// Private to the native runtime units (src/user/runtime.cpp and src/user/runtime/).
// The runtime is one serialized boot-CPU owner; every declaration here has
// exactly one definition in one unit, and all state is constant-initialized.
#include "ok/user/native_signal_config.hpp"
#include "ok/uapi/signal.h"
#include "../native_lifecycle.hpp"
#include "../signals/private_signal_scheduler_relocation.hpp"
#include "ok/user/native_signal_action.hpp"
#include "ok/user/runtime.hpp"
#include "ok/user/native_sessions_profile.hpp"
#include "ok/user/native_process_group_signal.hpp"
#include "ok/user/native_group_signal_profile.hpp"
#include "ok/user/native_console_control_profile.hpp"
#include "ok/core/native_shutdown.hpp"
#include "ok/user/native_sleep.hpp"
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
#define OK_PRIVATE_POLL_MASK 1
#include "../native_poll_mask.hpp"
#include "../signals/process_pending.hpp"
#include "../signals/wait_state.hpp"
#include "../signals/child_exit_lifecycle.hpp"
#include "ok/user/native_signal_target.hpp"
#include "ok/user/native_sleep_interruption.hpp"
#if defined(__i386__)
#include "ok/user/native_signal_delivery_i386.hpp"
#include "ok/user/native_signal_return_i386.hpp"
#elif defined(__arm__)
#include "ok/user/native_signal_delivery_arm32.hpp"
#include "ok/user/native_signal_return_arm32.hpp"
#elif defined(__aarch64__)
#include "ok/user/native_signal_delivery_aarch64.hpp"
#elif defined(__powerpc__) || defined(__PPC__)
#include "ok/user/native_signal_delivery_ppc32.hpp"
#include "ok/user/native_signal_return_ppc32.hpp"
#include "ok/user/native_signal_persistent_ppc32.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#define OK_PRIVATE_PPC32_SIGNAL 1
#elif defined(__mips64)
#include "ok/user/native_signal_delivery_mips64_n64.hpp"
#include "ok/user/native_signal_return_mips64_n64.hpp"
#include "ok/user/native_signal_gate_mips64_n64.hpp"
#include "ok/user/native_mips64_n64_elf_admission.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#define OK_PRIVATE_MIPS64_N64_SIGNAL 1
#elif defined(__mips__) && !defined(__mips64)
#include "ok/user/native_signal_delivery_mips_o32.hpp"
#include "ok/user/native_signal_return_mips_o32.hpp"
#include "ok/user/native_signal_gate_mips_o32.hpp"
#include "ok/user/native_mips_o32_elf_admission.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#define OK_PRIVATE_MIPS_O32_SIGNAL 1
#elif defined(__loongarch64)
#include "ok/user/native_signal_delivery_loongarch64.hpp"
#include "ok/user/native_signal_return_loongarch64.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#elif defined(__riscv)
#include "ok/user/native_signal_delivery_riscv.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#else
#include "ok/user/native_signal_delivery_x86_64.hpp"
#endif
#endif
#include "ok/user/native_poll.hpp"
#include "ok/user/native_futex.hpp"
#include "ok/user/native_futex_requeue_sdk_profile.hpp"
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
#include "../../../tests/native/private_futex_peer_shared.hpp"
#endif
#include "ok/user/native_select.hpp"
#include "ok/user/native_pty_pair_dispatch.hpp"
#include "ok/user/native_page_pool.hpp"
#include "ok/user/native_shared_files.hpp"
#include "ok/abi/linux_futex_wake_operation.hpp"
#include "ok/user/native_shared_files_sdk_profile.hpp"
#include "ok/user/native_entry_stacks.hpp"
#ifdef OK_KERNEL_FREESTANDING
#include "ok/arch/service_stack.hpp"
#endif
#include "../native_syscall_origin.hpp"
#include <ok/net/tcp_accept_copied.hpp>
#include "../native_shared_capture.hpp"
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
#include "../native_syscall_restart_publication.hpp"
#endif
#include "ok/memory/object_pool.hpp"
#include "ok/boot/qemu_process_memory.hpp"
#include "ok/abi/linux_exec.hpp"
#if defined(__x86_64__)
#include "ok/abi/linux_dynamic_exec_x86_64.hpp"
#endif
#include "ok/user/linux_bootstrap.hpp"
#include "ok/user/native_credentials.hpp"
#include "ok/user/native_mount_info.hpp"
#include "ok/user/console_control_dispatch.hpp"
#include "ok/user/native_metadata.hpp"
#include "ok/arch/execution.hpp"
#include "ok/arch/clock.hpp"
#include "ok/time/public_clock.hpp"
#include "ok/user/clock_set.hpp"
#include "ok/core/kernel.hpp"
#include "ok/net/native_udp_poll.hpp"
#include "../linux_udp_immediate_access.hpp"
#include "../linux_udp_captured_decode.hpp"
#include "../linux_udp_operation_body.hpp"
#include "../linux_udp_receive_owner_store.hpp"
#include "../tcp_immediate_access.hpp"
#include "../tcp_captured_accept.hpp"
#include "ok/sched/process.hpp"
#include "ok/syscall/linux.hpp"
#include "ok/uapi/syscall.h"

namespace ok::user::native
{
inline namespace runtime_detail
{
namespace hw = arch::execution;
namespace lu = abi::linux_user;
using LinuxExec = lu::ExecBootstrap<lu::configured_native_profile()>;
using Personality = sched::UserPersonality;
} // namespace runtime_detail
} // namespace ok::user::native
