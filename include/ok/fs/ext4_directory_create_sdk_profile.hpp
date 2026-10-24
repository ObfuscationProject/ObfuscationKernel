#pragma once

#include "ok/core/types.hpp"
#include "ok/core/module.hpp"
#include <string_view>

namespace ok::fs
{
inline constexpr std::string_view ext4_directory_create_profile = "bounded-journalled-single-block-directory-create-ordered-free-init-v2";
inline constexpr u32 ext4_directory_create_module_abi = 74;
inline constexpr usize ext4_directory_create_home_capacity = 8;
inline constexpr usize ext4_directory_create_owner_buffers = 9;
static_assert(ext4_directory_create_module_abi == kernel_module_abi_version);
static_assert(ext4_directory_create_home_capacity == 8);
static_assert(ext4_directory_create_owner_buffers == ext4_directory_create_home_capacity + 1);
} // namespace ok::fs
