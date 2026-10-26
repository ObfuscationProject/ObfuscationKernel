#pragma once

// Bounded volatile contract; separate acceptance receipts qualify execution.
#include <ok/core/module.hpp>
#include <ok/user/native_shared_files.hpp>
#include <string_view>
#include <type_traits>

namespace ok::user::native
{
inline constexpr std::string_view native_shared_file_profile = "bootcpu-volatile-sealed-size-write-eager-pages-v2";
inline constexpr std::string_view linux_shared_file_profile = "bootcpu-volatile-sealed-size-write-eager-pages-v2";
inline constexpr u32 native_shared_file_module_abi = 75;
inline constexpr usize native_shared_file_capacity = 16;
inline constexpr usize native_shared_file_max_pages = 64;
inline constexpr usize native_shared_file_page_size = 4096;
inline constexpr u32 native_shared_file_supported_seals = 15;

static_assert(kernel_module_abi_version == native_shared_file_module_abi);
static_assert(SharedMemoryFiles::capacity == native_shared_file_capacity);
static_assert(SharedMemoryFiles::max_pages == native_shared_file_max_pages);
static_assert(SharedMemoryFiles::page_size == native_shared_file_page_size);
static_assert(SharedMemoryFiles::supported_seals == native_shared_file_supported_seals);
static_assert(!std::is_copy_constructible_v<SharedMemoryFiles>);
static_assert(!std::is_move_constructible_v<SharedMemoryFiles>);
} // namespace ok::user::native
