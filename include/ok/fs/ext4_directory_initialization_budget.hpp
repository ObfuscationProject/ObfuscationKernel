#pragma once

#include "ok/block/sessions.hpp"
#include <array>

namespace ok::fs
{
// Pure reservation arithmetic for the bounded 1 KiB directory mutation route.
// The continuing Owner independently retains its original Session attachment,
// observes exact phase endpoints and quarantines every attempted-write failure.
// A copied report or successful reserve call grants no transport authority.
class Ext4DirectoryInitializationBudget final
{
  public:
    // G<=32, filesystem<=262145 blocks, inodes/group<=8192, N<=128
    // allocated ownership slots, eight homes and nine canonical participants.
    // The journal's contiguous extent independently limits parsing to 32767
    // reads. The directory-only format probe precedes the initial Completion.
    // An already-zero candidate prepares strict proposed replay here; a
    // nonzero candidate defers that same replay to initialization. These phase
    // maxima are mutually exclusive and must not be added as one execution.
    static constexpr u64 preparation_reads = 28'495'267'196ULL;
    static constexpr u64 initialization_reads = 14'247'682'313ULL;
    static constexpr u64 checkpoint_reads = 14'247'648'694ULL;
    // Includes the conservative pre-Owner mounted name/metadata prefix, even
    // when the real reservation starts after it, and all post-checkpoint inode
    // validation before drained rearm. Root cold-opening allowances are separate.
    static constexpr u64 continuation_reads = 14'281'666'318ULL;
    // Maximum complete branch: nonzero preparation + nonzero initialization +
    // checkpoint + continuation. Strict proposed replay is charged once.
    static constexpr u64 whole_reads = 57'024'615'847ULL;
    static constexpr u64 whole_writes = 22;
    static constexpr u64 whole_flushes = 10;

    template <block::SessionCounterPolicy Policy = block::SessionCounter>
    [[nodiscard]] static Status reserve(const block::SessionReport &initial)
    {
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            !initial.identity || !initial.registration_generation || !initial.session_serial ||
            initial.in_flight || initial.pending_writes)
            return Status::fault("directory initialization requires a healthy original Session");
        const std::array<block::SessionCounter, 6> counts{
            initial.reads, initial.completed_reads, initial.writes,
            initial.completed_writes, initial.flushes, initial.completed_flushes};
        constexpr std::array<block::SessionCounter, 6> reserved{
            whole_reads, whole_reads, whole_writes, whole_writes, whole_flushes, whole_flushes};
        for (usize index = 0; index < counts.size(); ++index)
            if (!block::session_counter_fits<Policy>(counts[index], reserved[index]))
                return Status::overflow("directory initialization six-counter headroom exhausted");
        if (initial.reads != initial.completed_reads || initial.writes != initial.completed_writes ||
            initial.flushes != initial.completed_flushes)
            return Status::fault("directory initialization requires completed counter parity");
        return {};
    }
};
} // namespace ok::fs
