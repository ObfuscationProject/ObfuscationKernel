#pragma once

// SDK67 source capability for non-PI FUTEX_REQUEUE and FUTEX_CMP_REQUEUE.
// READ keys, signed32-bit counts and the exact original WAIT continuation are
// preserved under boot-CPU serialization. Shared anonymous transfer pins retain
// explicit owners across preflight and failure. Same-key transfers preserve
// queue sequence and select every captured waiter at most once.
// These declarations do not qualify native guest execution, SMP, Linux bucket
// priority, PI, realtime scheduling, file-backed keys or cross-process CLONE_VM.
#include <ok/core/module.hpp>
#include <ok/user/native_futex.hpp>
#include <ok/sched/scheduler.hpp>
#include <string_view>
#include <type_traits>

namespace ok::user::native
{
inline constexpr std::string_view native_futex_requeue_profile = "bootcpu-nonpi-read-keys-signed-counts-exact-origin-ticket-sequence-owned-transfer-v1";
inline constexpr std::string_view linux_futex_requeue_profile = "bootcpu-nonpi-read-keys-signed-counts-exact-origin-ticket-sequence-owned-transfer-v1";
inline constexpr u32 native_futex_requeue_module_abi = 77;
inline constexpr usize native_futex_requeue_word_bits = 32;
inline constexpr usize native_futex_requeue_operation_count = 2;
inline constexpr usize native_futex_requeue_linux_operation = 3;
inline constexpr usize native_futex_cmp_requeue_linux_operation = 4;
inline constexpr usize native_futex_requeue_private_flag = 128;
inline constexpr usize native_futex_requeue_key_domains = 2;
inline constexpr usize native_futex_requeue_waiter_capacity = 64;
inline constexpr usize native_futex_requeue_sequence_bits = 64;
inline constexpr u32 native_futex_requeue_original_continuation_preserved = 1;
inline constexpr u32 native_futex_requeue_same_key_sequence_preserved = 1;
inline constexpr u32 native_futex_requeue_zero_wake_minimum = 0;
inline constexpr u32 native_futex_requeue_signed_negative_counts_rejected = 1;
inline constexpr u32 native_futex_requeue_transfer_pin_owners_retained = 1;
inline constexpr u32 native_futex_requeue_smp_atomic = 0;
inline constexpr u32 native_futex_requeue_priority_inheritance = 0;
inline constexpr u32 native_futex_requeue_realtime_priority = 0;
inline constexpr u32 native_futex_requeue_file_backed = 0;
inline constexpr u32 native_futex_requeue_cross_process_clone_vm = 0;

static_assert(kernel_module_abi_version == native_futex_requeue_module_abi);
static_assert(sizeof(u32) * 8 == native_futex_requeue_word_bits);
static_assert(sizeof(i32) * 8 == native_futex_requeue_word_bits);
static_assert(sizeof(u64) * 8 == native_futex_requeue_sequence_bits);
static_assert(sched::max_processes == native_futex_requeue_waiter_capacity);
static_assert(std::is_same_v<decltype(FutexRequeueRequest::source_address), uptr>);
static_assert(std::is_same_v<decltype(FutexRequeueRequest::destination_address), uptr>);
static_assert(std::is_same_v<decltype(FutexRequeueRequest::wake_limit), u32>);
static_assert(std::is_same_v<decltype(FutexRequeueRequest::requeue_limit), u32>);
static_assert(std::is_same_v<decltype(FutexRequeueRequest::expected), u32>);
static_assert(std::is_same_v<decltype(FutexRequeueRequest::compare), bool>);
static_assert(std::is_same_v<decltype(&futex_requeue),
                           i64 (*)(Kernel &, sched::ProcessId, const FutexRequeueRequest &)>);
static_assert(std::is_standard_layout_v<FutexRequeueRequest>);
static_assert(std::is_trivially_copyable_v<FutexRequeueRequest>);
static_assert(static_cast<u8>(FutexKeyDomain::private_address_space) == 0);
static_assert(static_cast<u8>(FutexKeyDomain::anonymous_shared_operation) + 1 ==
              native_futex_requeue_key_domains);
} // namespace ok::user::native
