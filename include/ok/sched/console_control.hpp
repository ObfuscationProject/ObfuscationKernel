#pragma once

#include "ok/core/types.hpp"

namespace ok::sched
{
enum class ConsoleControlOperation : u8
{
    acquire,
    get_session,
    get_foreground,
    set_foreground,
};
} // namespace ok::sched
