#pragma once

// Private to src/sched: resource-drain accounting shared by scheduler.cpp and
// descriptions.cpp. Internal linkage in each unit; not part of the SDK.
#include "ok/sched/scheduler.hpp"

namespace ok::sched
{
namespace
{

void count_resource_event(usize &count)
{
    if (count != ~usize{0}) ++count;
}

struct ResourceDrainAdmission
{
    bool &active;
    ~ResourceDrainAdmission() { active = false; }
};

} // namespace
} // namespace ok::sched
