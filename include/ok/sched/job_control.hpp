#pragma once

#include "ok/core/types.hpp"

namespace ok::sched
{

// Per-process stop overlay. ProcessState stays unchanged: waits a stopped
// process started still complete; the overlay only keeps it from being entered.
struct JobControlState
{
    u8 stop_signal{};          // In the process's own numbering, while stopped or reported.
    bool stopped{};
    bool stop_report{};        // Owed to WUNTRACED/WSTOPPED.
    bool continue_report{};    // Owed to WCONTINUED.
    u32 continue_generation{}; // Bumped by every continue.
    constexpr bool operator==(const JobControlState &) const = default;
};

struct JobMember
{
    u64 pid{}, parent{}, group{}, session{};
    bool live{};
};

// Linux will_become_orphaned_pgrp: the group is orphaned unless a live member
// other than `ignored` has a parent outside the group but inside the member's
// session. A parent that is `init` never holds a group. `project` maps an
// element of `processes` to a JobMember.
template <typename Processes, typename Project>
[[nodiscard]] constexpr bool job_group_orphaned(const Processes &processes, u64 group, u64 ignored, u64 init,
                                                Project &&project)
{
    for (const auto &candidate : processes)
    {
        const JobMember member = project(candidate);
        if (member.group != group || !member.live || member.pid == ignored || member.parent == init) continue;
        for (const auto &other : processes)
        {
            const JobMember parent = project(other);
            if (parent.pid != member.parent) continue;
            if (parent.group != group && parent.session == member.session) return false;
            break;
        }
    }
    return true;
}

} // namespace ok::sched
