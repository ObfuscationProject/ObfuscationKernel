#pragma once

#include "ok/block/sessions.hpp"
#include "ok/fs/ext4_recovery.hpp"
#include <type_traits>

namespace ok::fs
{
enum class Ext4ZeroTruncateRoute : u8 { fresh, recovery, noop };
struct Ext4ZeroTruncateTransactionCapacity
{
    block::SessionCounter reads{}, writes{}, flushes{};
    u32 groups{}, final_sequence{};
    Ext4ZeroTruncateRoute route{};
    friend bool operator==(const Ext4ZeroTruncateTransactionCapacity &,
                           const Ext4ZeroTruncateTransactionCapacity &) = default;
};

// Pure reservation arithmetic. Its returned facts grant no I/O authority.
class Ext4ZeroTruncateTransactionBudget final
{
  public:
    template <block::SessionCounterPolicy Counter = block::SessionCounter>
    [[nodiscard]] static Result<Ext4ZeroTruncateTransactionCapacity> capacity(
        const block::SessionReport &initial, u32 groups, Ext4ZeroTruncateRoute route, u32 sequence)
    {
        if (!groups || groups > Ext4RecoveryInfo::group_capacity || !sequence ||
            (route != Ext4ZeroTruncateRoute::fresh && route != Ext4ZeroTruncateRoute::recovery &&
             route != Ext4ZeroTruncateRoute::noop))
            return Status::invalid_argument("zero truncate whole route or group capacity");
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            !initial.identity || !initial.registration_generation || !initial.session_serial ||
            initial.in_flight || initial.pending_writes || initial.reads != initial.completed_reads ||
            initial.writes != initial.completed_writes || initial.flushes != initial.completed_flushes)
            return Status::fault("zero truncate whole route requires a stable original Session");
        Ext4ZeroTruncateTransactionCapacity result{};
        result.groups = groups; result.route = route; result.final_sequence = sequence;
        // A=1420+531G: one strict opening and full ownership proof.
        // B=2866+1062G: accepted active preview, including its captures.
        if (route == Ext4ZeroTruncateRoute::fresh)
        {
            if (sequence > 0xfffffffcU) return Status::overflow("zero truncate fresh sequence reservation exhausted");
            result.reads = 5849 + 2124 * block::SessionCounter(groups); // B+2A+13+10*13.
            result.writes = 13; result.flushes = 6; result.final_sequence += 2;
        }
        else if (route == Ext4ZeroTruncateRoute::recovery)
        {
            if (sequence < 2 || sequence > 0xfffffffdU)
                return Status::overflow("zero truncate recovery sequence reservation exhausted");
            result.reads = 4351 + 1593 * block::SessionCounter(groups); // B+A+5*13.
            result.writes = 5; result.flushes = 3; ++result.final_sequence;
        }
        else result.reads = 1446 + 531 * block::SessionCounter(groups); // A+13+13.
        const auto fits = [](block::SessionCounter initial_count, block::SessionCounter count) {
            return block::session_counter_fits<Counter>(initial_count, count);
        };
        if (!fits(initial.reads, result.reads) || !fits(initial.completed_reads, result.reads) ||
            !fits(initial.writes, result.writes) || !fits(initial.completed_writes, result.writes) ||
            !fits(initial.flushes, result.flushes) || !fits(initial.completed_flushes, result.flushes))
            return Status::overflow("zero truncate whole attempted/completed counters exhausted");
        return result;
    }
};
} // namespace ok::fs
