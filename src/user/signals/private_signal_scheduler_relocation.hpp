#pragma once

#include "ok/sched/scheduler.hpp"

// Runs before every reap, including GUI/service initiated ones. Process slots
// never move, so nothing is relocated; departed Linux UDP receives retire
// here. This exports no SDK layout or API.
namespace ok::user::native
{
Status private_signal_before_process_reap(sched::SchedulerState &scheduler);
}
