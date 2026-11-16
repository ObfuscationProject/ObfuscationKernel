#pragma once
#include "ok/core/types.hpp"
#include <string_view>
namespace ok::sched
{
inline constexpr std::string_view native_console_input_profile = "pid1-issued-epoch-fork-exec-leader-revoke-ntty-compat-default-v2";
inline constexpr u32 native_console_epoch_limit = 0x3fffffff;
} // namespace ok::sched
