#pragma once

#include "ok/core/types.hpp"

namespace ok
{

enum class SystemPowerAction : u8
{
    none,
    halt,
    poweroff,
    reboot,
};

} // namespace ok
