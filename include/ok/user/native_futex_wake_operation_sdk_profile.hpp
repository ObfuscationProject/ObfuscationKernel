#pragma once

// Source capability contract for FUTEX_WAKE_OP. The kernel lock serializes the
// futex queues on every CPU; the second word's update is one atomic
// read-modify-write of its backing frame (after copy-on-write), so user atomics
// on other CPUs are neither lost nor see torn bytes. i386 runs UP and updates
// the word with plain aligned accesses while no user code runs. Signed
// twelve-bit operands and comparisons operate on unsigned wrapping words;
// nonpositive signed wake counts retain the legacy one-waiter-per-pass rule.
// Private and canonical shared-anonymous keys retain separate domains.
// The two request domains also preserve ordinary private-backing local keys;
// the key-domain marker counts request choices, not backing-key variants.
// This is not lock-free futex queues, file-backed keys or cross-process CLONE_VM.
// Hosted models and exported metadata do not qualify native guest execution.
#include <ok/core/module.hpp>
#include <ok/abi/linux_futex_wake_operation.hpp>
#include <ok/user/native_futex.hpp>
#include <string_view>
#include <type_traits>

namespace ok::user::native
{
inline constexpr std::string_view native_futex_wake_operation_profile = "kernel-lock-atomic-word-two-key-private-anonymous-shared-signed-wrap-legacy-count-v2";
inline constexpr std::string_view linux_futex_wake_operation_profile = "kernel-lock-atomic-word-two-key-private-anonymous-shared-signed-wrap-legacy-count-v2";
inline constexpr u32 native_futex_wake_operation_module_abi = 80;
inline constexpr usize native_futex_wake_operation_word_bits = 32;
inline constexpr usize native_futex_wake_operation_operand_bits = 12;
inline constexpr usize native_futex_wake_operation_shift_bits = 5;
inline constexpr usize native_futex_wake_operation_arithmetic_count = 5;
inline constexpr usize native_futex_wake_operation_comparison_count = 6;
inline constexpr usize native_futex_wake_operation_wake_passes = 2;
inline constexpr usize native_futex_wake_operation_key_domains = 2;
inline constexpr usize native_futex_wake_operation_nonpositive_wake_minimum = 1;
// 1: atomic with respect to every CPU that can run user code on this profile
// (a hardware RMW, or plain access on a profile that starts no secondary CPU).
inline constexpr u32 native_futex_wake_operation_smp_atomic = 1;
inline constexpr u32 native_futex_wake_operation_file_backed = 0;
inline constexpr u32 native_futex_wake_operation_cross_process_clone_vm = 0;

static_assert(kernel_module_abi_version == native_futex_wake_operation_module_abi);
static_assert(sizeof(u32) * 8 == native_futex_wake_operation_word_bits);
static_assert(sizeof(i32) * 8 == native_futex_wake_operation_word_bits);
static_assert(std::is_same_v<decltype(FutexWakeOperationRequest::first_limit), u32>);
static_assert(std::is_same_v<decltype(FutexWakeOperationRequest::second_limit), u32>);
static_assert(std::is_same_v<decltype(FutexWakeOperationRequest::encoded_operation), u32>);
static_assert(std::is_same_v<decltype(&futex_wake_operation),
                           i64 (*)(Kernel &, sched::ProcessId, const FutexWakeOperationRequest &)>);
static_assert(std::is_trivially_copyable_v<FutexWakeOperationRequest>);
static_assert(std::is_standard_layout_v<FutexWakeOperationRequest>);
static_assert(static_cast<u8>(FutexKeyDomain::private_address_space) == 0);
static_assert(static_cast<u8>(FutexKeyDomain::anonymous_shared_operation) + 1 ==
              native_futex_wake_operation_key_domains);
static_assert(std::is_same_v<decltype(abi::linux_user::FutexWakeOperation::operand), i32>);
static_assert(std::is_same_v<decltype(abi::linux_user::FutexWakeOperation::compare_operand), i32>);
static_assert(abi::linux_user::FutexWakeOperation::signed_operand(0x7ffU) == 2047);
static_assert(abi::linux_user::FutexWakeOperation::signed_operand(0x800U) == -2048);
static_assert(abi::linux_user::FutexWakeOperation::signed_operand(0xfffU) == -1);
} // namespace ok::user::native
