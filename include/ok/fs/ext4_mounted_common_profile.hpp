#pragma once

#include "ok/block/session_counters.hpp"
#include "ok/core/module.hpp"
#include "ok/fs/ext4/mount.hpp"
#include <string_view>
#include <type_traits>

namespace ok::fs
{
// Serialized eight-family startup, bounded directory creation, zero/refill and
// canonical legacy writes on checksum-v3 media with no active revokes.
inline constexpr std::string_view ext4_common_opening_profile = "serialized-common-eight-family-directory-create-ordered-free-init-zero-refill-optional-revoke-legacy-v4";
inline constexpr u32 ext4_common_module_abi = 74;
inline constexpr u32 ext4_common_session_counter_bits = 64;

static_assert(kernel_module_abi_version == ext4_common_module_abi);
static_assert(std::is_same_v<decltype(jbd2::ReplayPlan{}.revoke_records), u32>);
static_assert(sizeof(block::SessionCounter) * 8 == ext4_common_session_counter_bits);
using CommonProfileMount = Ext4MountedMetadata<1>;
static_assert(!std::is_copy_constructible_v<CommonProfileMount::CommonStorage>);
static_assert(!std::is_move_constructible_v<CommonProfileMount::CommonStorage>);
static_assert(std::is_constructible_v<CommonProfileMount, CommonProfileMount::CommonStorage &>);
static_assert(!std::is_constructible_v<CommonProfileMount, CommonProfileMount::CommonStorage &&>);
}
