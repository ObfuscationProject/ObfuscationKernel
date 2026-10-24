#pragma once

#include "ok/sched/scheduler.hpp"

// Private debug fixture side state follows every canonical process compaction,
// including GUI/service initiated reaping. This exports no SDK layout or API.
namespace ok::user::native
{
Status private_signal_before_process_compaction(sched::SchedulerState &scheduler);
Status private_signal_after_process_compaction(sched::SchedulerState &scheduler);
}
