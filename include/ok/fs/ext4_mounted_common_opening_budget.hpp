#pragma once

#include "ok/block/session_counters.hpp"
#include <array>

namespace ok::fs
{
enum class Ext4MountedCommonFamily : u8
{ ordinary, legacy, empty_orphan, namespace_recovery, reclaim, typed_zero, typed_allocated, directory_create, regular_move };

struct Ext4MountedCommonCapacity
{
    block::SessionCounter reads{}, writes{}, flushes{};
    u32 sequence_delta{};
    u8 locator_reads{};
    friend bool operator==(const Ext4MountedCommonCapacity &, const Ext4MountedCommonCapacity &) = default;
};

// Fixed G4/F32768 arithmetic only. Neither operands nor results authenticate a
// family, a Session endpoint, a retained opening or permission to issue I/O.
class Ext4MountedCommonOpeningBudget final
{
  public:
    using Counter = block::SessionCounter;
    // Attempted/completed READ, WRITE and FLUSH, in that order.
    using Counts = std::array<Counter, 6>;
    using Family = Ext4MountedCommonFamily;
    using Capacity = Ext4MountedCommonCapacity;

    [[nodiscard]] static Result<Capacity> route(Family family)
    {
        const auto index = static_cast<usize>(family);
        if (index >= routes_.size()) return Status::invalid_argument("common opening budget family");
        return routes_[index];
    }

    [[nodiscard]] static constexpr Capacity whole_union()
    {
        Capacity out{};
        for (const auto &row : routes_)
        {
            if (row.reads > out.reads) out.reads = row.reads;
            if (row.writes > out.writes) out.writes = row.writes;
            if (row.flushes > out.flushes) out.flushes = row.flushes;
        }
        // Sequence and locator are learned from the selected real family.
        return out;
    }

    template <block::SessionCounterPolicy Policy = Counter>
    [[nodiscard]] static constexpr Status reserve(const Counts &baseline)
    {
        const auto capacities = fields(whole_union());
        for (usize index = 0; index < baseline.size(); ++index)
            if (!block::session_counter_fits<Policy>(baseline[index], capacities[index]))
                return Status::overflow("common opening six-counter union exhausted");
        return {};
    }

    template <block::SessionCounterPolicy Policy = Counter>
    [[nodiscard]] static Result<Capacity> narrow(const Counts &baseline,
        const Counts &endpoint, Family family, u8 classifier_reads)
    {
        if (auto status = reserve<Policy>(baseline); !status) return status;
        const auto selected = route(family);
        if (!selected) return selected.status();
        const auto capacity = selected.value();
        if (classifier_reads != capacity.locator_reads)
            return Status::invalid_argument("common opening locator count differs from family");
        for (usize index = 0; index < baseline.size(); ++index)
        {
            const Counter spent = index < 2 ? classifier_reads : 0;
            if (endpoint[index] < baseline[index] || endpoint[index] - baseline[index] != spent)
                return Status::invalid_argument("common opening classifier counter delta");
        }
        if (baseline[0] != baseline[1] || baseline[2] != baseline[3] || baseline[4] != baseline[5])
            return Status::invalid_argument("common opening baseline requires completed parity");
        if (classifier_reads > capacity.reads)
            return Status::overflow("common opening locator exceeds selected route");
        auto remaining = capacity;
        remaining.reads -= classifier_reads;
        const auto capacities = fields(remaining);
        for (usize index = 0; index < endpoint.size(); ++index)
            if (!block::session_counter_fits<Policy>(endpoint[index], capacities[index]))
                return Status::overflow("common opening remaining route exhausted");
        return remaining;
    }

  private:
    [[nodiscard]] static constexpr Counts fields(const Capacity &capacity)
    {
        return {capacity.reads, capacity.reads, capacity.writes, capacity.writes,
                capacity.flushes, capacity.flushes};
    }

    // Source-derived upper bounds, including the locator exactly once and the
    // complete terminal/root continuation. Their derivation is pinned in
    // docs/storage/common-opening-budget.md; tests use that independent oracle.
    inline static constexpr std::array<Capacity, 9> routes_{{
        {58'811'342'498ULL, 8, 6, 2, 4},
        {23'524'424'765ULL, 7, 5, 2, 5},
        {11'762'325'977ULL, 120, 64, 24, 4},
        {11'762'191'265ULL, 21, 11, 5, 5},
        {23'524'315'345ULL, 111, 61, 23, 5},
        {2'917'720'020ULL, 5, 3, 1, 5},
        {2'917'720'035ULL, 5, 3, 1, 5},
        {23'524'132'523ULL, 9, 5, 2, 5},
        // 5 locator + 143 move + 1 final head + inherited terminal/root
        // continuation (11,762,118,846). No orphan chain follows this family.
        {11'762'118'995ULL, 6, 3, 2, 5},
    }};
};
} // namespace ok::fs
