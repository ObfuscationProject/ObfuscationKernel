#pragma once

#include "ok/block/sessions.hpp"
#include "ok/fs/ext4_recovery.hpp"
#include <type_traits>

namespace ok::fs
{
enum class Ext4AllocatedEmptyDataRoute : u8 { fresh, recovery };
struct Ext4AllocatedEmptyDataTransactionCapacity
{
    block::SessionCounter reads{}, writes{}, flushes{};
    u32 groups{}, final_sequence{};
    Ext4AllocatedEmptyDataRoute route{};
    friend bool operator==(const Ext4AllocatedEmptyDataTransactionCapacity &,
                           const Ext4AllocatedEmptyDataTransactionCapacity &) = default;
};

// Pure whole-route arithmetic; no returned fact grants transport authority.
class Ext4AllocatedEmptyDataTransactionBudget final
{
  public:
    template <block::SessionCounterPolicy Counter = block::SessionCounter>
    [[nodiscard]] static Result<Ext4AllocatedEmptyDataTransactionCapacity> capacity(
        const block::SessionReport &initial, u32 groups, Ext4AllocatedEmptyDataRoute route, u32 sequence)
    {
        if (!groups || groups > Ext4RecoveryInfo::group_capacity || !sequence ||
            (route != Ext4AllocatedEmptyDataRoute::fresh && route != Ext4AllocatedEmptyDataRoute::recovery))
            return Status::invalid_argument("allocated empty whole route or group capacity");
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            !initial.identity || !initial.registration_generation || !initial.session_serial ||
            initial.in_flight || initial.pending_writes || initial.reads != initial.completed_reads ||
            initial.writes != initial.completed_writes || initial.flushes != initial.completed_flushes)
            return Status::fault("allocated empty whole route requires a stable original Session");
        Ext4AllocatedEmptyDataTransactionCapacity result{};
        result.groups = groups; result.route = route; result.final_sequence = sequence;
        // A=1420+531G: checked opening plus complete ownership.
        // B=2876+1062G: active allocated-empty preview.
        if (route == Ext4AllocatedEmptyDataRoute::fresh)
        {
            if (sequence > 0xfffffffcU)
                return Status::overflow("allocated empty fresh sequence reservation exhausted");
            result.reads = 5884 + 2125 * block::SessionCounter(groups); // B+2A+G+14+11*14.
            result.writes = 14; result.flushes = 7; result.final_sequence += 2;
        }
        else
        {
            if (sequence < 2 || sequence > 0xfffffffdU)
                return Status::overflow("allocated empty recovery sequence reservation exhausted");
            result.reads = 4366 + 1593 * block::SessionCounter(groups); // B+A+5*14.
            result.writes = 5; result.flushes = 3; ++result.final_sequence;
        }
        const auto fits = [](block::SessionCounter used, block::SessionCounter count) {
            return block::session_counter_fits<Counter>(used, count);
        };
        if (!fits(initial.reads, result.reads) || !fits(initial.completed_reads, result.reads) ||
            !fits(initial.writes, result.writes) || !fits(initial.completed_writes, result.writes) ||
            !fits(initial.flushes, result.flushes) || !fits(initial.completed_flushes, result.flushes))
            return Status::overflow("allocated empty whole attempted/completed counters exhausted");
        return result;
    }
};
} // namespace ok::fs
