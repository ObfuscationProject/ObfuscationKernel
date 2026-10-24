#pragma once

#include "ok/fs/ext4_reclaim_history.hpp"

namespace ok::fs
{
template <usize> class Ext4ReclaimComposedHistory;
enum class Ext4ReclaimDryPhase : u8 { capture, plan, ordinary, terminal, unused };

// Diagnostic bounds for the concrete read-only route; never a write token.
struct Ext4ReclaimComposedReadCapacity
{
    std::array<block::SessionCounter, 4> per_pass{}, cumulative{};
    block::SessionCounter extension{}, aggregate{};
    usize remaining{};
    u32 projected_sequence_end{};
};

class Ext4ReclaimComposedReadBudget final
{
    template <usize> friend class Ext4ReclaimComposedHistory;
  public:
    Ext4ReclaimComposedReadBudget() = default;
    Ext4ReclaimComposedReadBudget(const Ext4ReclaimComposedReadBudget &) = delete;
    Ext4ReclaimComposedReadBudget &operator=(const Ext4ReclaimComposedReadBudget &) = delete;
    Ext4ReclaimComposedReadBudget(Ext4ReclaimComposedReadBudget &&) = delete;
    Ext4ReclaimComposedReadBudget &operator=(Ext4ReclaimComposedReadBudget &&) = delete;

    [[nodiscard]] static Result<Ext4ReclaimComposedReadCapacity> capacity(
        const block::SessionReport &initial, usize remaining, bool extension, u32 sequence)
    {
        if (remaining > 7) return Status::unsupported("composed dry remaining-member capacity");
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            initial.in_flight || initial.pending_writes || initial.reads != initial.completed_reads ||
            initial.writes != initial.completed_writes || initial.flushes != initial.completed_flushes)
            return Status::fault("composed dry reservation requires stable original counters");
        Ext4ReclaimComposedReadCapacity result;
        result.remaining = remaining; result.extension = extension ? 1 : 0;
        result.per_pass = {4654, 2327, 10163275, 2335};
        result.cumulative = result.per_pass;
        for (usize phase = 0; phase < 2; ++phase)
        {
            if (remaining && result.per_pass[phase] > ~block::SessionCounter{0} / remaining)
                return Status::overflow("composed dry stage reservation multiplication");
            result.cumulative[phase] = result.per_pass[phase] * remaining;
        }
        result.aggregate = result.extension;
        for (block::SessionCounter count : result.cumulative)
        {
            if (result.aggregate > ~block::SessionCounter{0} - count)
                return Status::overflow("composed dry aggregate reservation addition");
            result.aggregate += count;
        }
        if (initial.reads > ~block::SessionCounter{0} - result.aggregate ||
            initial.completed_reads > ~block::SessionCounter{0} - result.aggregate)
            return Status::overflow("composed complete dry READ reservation exhausted");
        if (remaining > (~u32{0} - 2) / 3 || sequence > ~u32{0} - 2 - u32(3 * remaining))
            return Status::overflow("composed dry projected sequence reservation exhausted");
        result.projected_sequence_end = sequence + 2 + u32(3 * remaining);
        return result;
    }

  private:
    static bool same_capacity(const Ext4ReclaimComposedReadCapacity &a,
                              const Ext4ReclaimComposedReadCapacity &b)
    {
        return a.per_pass == b.per_pass && a.cumulative == b.cumulative &&
            a.extension == b.extension && a.aggregate == b.aggregate &&
            a.remaining == b.remaining && a.projected_sequence_end == b.projected_sequence_end;
    }
    [[nodiscard]] Status reserve(const block::SessionReport &initial, usize remaining,
                                 bool extension, u32 sequence)
    {
        if (attempted_) return Status::already_exists("composed dry reservation is one-shot");
        attempted_ = true;
        const auto result = capacity(initial, remaining, extension, sequence);
        if (!result) return result.status();
        capacity_ = sealed_ = result.value(); ready_ = true;
        return {};
    }
    [[nodiscard]] Status exact(const block::SessionReport &initial, usize remaining,
                               bool extension, u32 sequence) const
    {
        if (!attempted_ || !ready_ || !same_capacity(capacity_, sealed_))
            return Status::fault("composed sealed dry reservation changed");
        const auto expected = capacity(initial, remaining, extension, sequence);
        if (!expected) return expected.status();
        return same_capacity(capacity_, expected.value()) ? Status{} :
            Status::fault("composed dry reservation differs from original route facts");
    }
    [[nodiscard]] block::SessionCounter per_pass(Ext4ReclaimDryPhase phase) const
    { return usize(phase) < capacity_.per_pass.size() ? capacity_.per_pass[usize(phase)] : 0; }
    [[nodiscard]] block::SessionCounter cumulative(Ext4ReclaimDryPhase phase) const
    { return usize(phase) < capacity_.cumulative.size() ? capacity_.cumulative[usize(phase)] : 0; }
    [[nodiscard]] Status charges(block::SessionCounter extension, const std::array<block::SessionCounter, 4> &stages, block::SessionCounter reads) const
    {
        if (!ready_ || extension > capacity_.extension)
            return Status::overflow("composed dry extension reservation exhausted");
        block::SessionCounter aggregate = extension;
        for (usize phase = 0; phase < stages.size(); ++phase)
        {
            if (stages[phase] > capacity_.cumulative[phase] || aggregate > ~block::SessionCounter{0} - stages[phase])
                return Status::overflow("composed dry stage reservation exhausted");
            aggregate += stages[phase];
        }
        if (aggregate > capacity_.aggregate)
            return Status::overflow("composed dry aggregate reservation exhausted");
        return aggregate == reads ? Status{} : Status::fault("composed dry reserved charges differ from Session reads");
    }
    Ext4ReclaimComposedReadCapacity capacity_{}, sealed_{};
    bool attempted_{}, ready_{};
};
} // namespace ok::fs
