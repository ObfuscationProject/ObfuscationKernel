#pragma once

#include "ok/block/sessions.hpp"
#include "ok/fs/ext4_recovery.hpp"
#include "ok/fs/ext4_directory_create_sdk_profile.hpp"
#include <type_traits>

namespace ok::fs
{
enum class Ext4MountedTypedRoute : u8
{ zero_fresh, zero_recovery, zero_noop, allocated_fresh, allocated_recovery };
enum class Ext4MountedTypedOrigin : u8 { mutation, startup };

struct Ext4MountedTypedBounds
{
    u64 decode{}, view_open{}, stored_metadata{}, completion{}, strict_empty{};
    u64 mutation_prefix{}, mutation_suffix{}, startup_suffix{}, directory_prefix_sum{};
    friend bool operator==(const Ext4MountedTypedBounds &, const Ext4MountedTypedBounds &) = default;
};
struct Ext4MountedTypedCapacity
{
    u64 classifier_reads{}, owner_prefix_reads{}, child_reads{}, suffix_reads{};
    u64 reads{}, writes{}, flushes{};
    u32 groups{}, sequence_delta{};
    u64 filesystem_blocks{};
    Ext4MountedTypedRoute route{};
    Ext4MountedTypedOrigin origin{};
    friend bool operator==(const Ext4MountedTypedCapacity &, const Ext4MountedTypedCapacity &) = default;
};

// Inert reservation facts. They grant no role, Session, child or writer authority.
class Ext4MountedTypedBudget final
{
  public:
    [[nodiscard]] static Result<Ext4MountedTypedBounds> bounds(u32 groups, u64 filesystem_blocks)
    {
        if (!groups || groups > Ext4RecoveryInfo::group_capacity || !filesystem_blocks)
            return Status::invalid_argument("mounted typed budget geometry");
        Ext4MountedTypedBounds out{};
        u64 scaled{}, coefficient{}, terms{};
        if (!multiply(4, groups, scaled) || !add(2, scaled, out.decode) ||
            !multiply(2, out.decode, scaled) || !add(1026, scaled, out.view_open) ||
            !multiply(129, out.decode, scaled) || !add(groups, scaled, out.stored_metadata))
            return Status::overflow("mounted typed decoder budget overflow");
        out.directory_prefix_sum = filesystem_blocks < 129 * u64{1024} ? filesystem_blocks : 129 * u64{1024};
        // Each prefix includes its first attempted failing mapped data block.
        // Its aggregate bound requires the actual continuing child ownership proof.
        if (!multiply(168, out.decode, scaled) || !add(86017, scaled, coefficient) ||
            !multiply(coefficient, out.directory_prefix_sum, terms) ||
            !multiply(515, out.decode, scaled) || !add(groups, scaled, out.completion) ||
            !add(out.completion, terms, out.completion) || !multiply(2, groups, scaled) ||
            !add(7, scaled, out.strict_empty) || !add(out.strict_empty, out.view_open, out.strict_empty) ||
            !add(out.strict_empty, out.completion, out.strict_empty) ||
            !multiply(2, out.decode, scaled) || !add(scaled, 1, out.mutation_prefix) ||
            !add(out.strict_empty, out.view_open, terms) || !add(terms, out.stored_metadata, terms) ||
            !add(terms, scaled, out.mutation_suffix) || !add(out.mutation_suffix, 2, out.mutation_suffix) ||
            !add(out.mutation_suffix, out.decode, out.startup_suffix))
            return Status::overflow("mounted typed suffix budget overflow");
        return out;
    }

    template <typename Counter = block::SessionCounter>
        requires block::SessionCounterPolicy<Counter>
    [[nodiscard]] static Status counters(const block::SessionReport &initial,
                                         const Ext4MountedTypedCapacity &capacity)
    {
        constexpr u64 limit = u64(Counter(~Counter{0}));
        const auto fits = [limit](block::SessionCounter used, u64 reserved) {
            return u64(used) <= limit && reserved <= limit - u64(used);
        };
        if (!fits(initial.reads, capacity.reads) || !fits(initial.completed_reads, capacity.reads) ||
            !fits(initial.writes, capacity.writes) || !fits(initial.completed_writes, capacity.writes) ||
            !fits(initial.flushes, capacity.flushes) || !fits(initial.completed_flushes, capacity.flushes))
            return Status::overflow("mounted typed whole attempted/completed counters exhausted");
        return {};
    }

    template <typename Counter = block::SessionCounter>
        requires block::SessionCounterPolicy<Counter>
    [[nodiscard]] static Result<Ext4MountedTypedCapacity> capacity(
        const block::SessionReport &initial, u32 groups, u64 filesystem_blocks,
        Ext4MountedTypedOrigin origin, Ext4MountedTypedRoute route)
    {
        if ((origin != Ext4MountedTypedOrigin::mutation && origin != Ext4MountedTypedOrigin::startup) ||
            (route != Ext4MountedTypedRoute::zero_fresh && route != Ext4MountedTypedRoute::zero_recovery &&
             route != Ext4MountedTypedRoute::zero_noop && route != Ext4MountedTypedRoute::allocated_fresh &&
             route != Ext4MountedTypedRoute::allocated_recovery))
            return Status::invalid_argument("mounted typed budget route");
        if (origin == Ext4MountedTypedOrigin::startup &&
            (groups != 4 || filesystem_blocks != 32768 ||
             (route != Ext4MountedTypedRoute::zero_recovery && route != Ext4MountedTypedRoute::allocated_recovery)))
            return Status::unsupported("mounted typed startup fixed locator profile");
        if (initial.state != block::DeviceState::owned || initial.first_error != StatusCode::ok ||
            !initial.identity || !initial.registration_generation || !initial.session_serial ||
            initial.in_flight || initial.pending_writes || initial.reads != initial.completed_reads ||
            initial.writes != initial.completed_writes || initial.flushes != initial.completed_flushes)
            return Status::fault("mounted typed budget requires a stable original Session");
        const auto computed = bounds(groups, filesystem_blocks);
        if (!computed) return computed.status();
        const auto &bound = computed.value();
        Ext4MountedTypedCapacity out{};
        out.groups = groups; out.filesystem_blocks = filesystem_blocks; out.origin = origin; out.route = route;
        out.classifier_reads = origin == Ext4MountedTypedOrigin::startup ? 5 : bound.mutation_prefix;
        // The original Owner rereads every resident canonical slot before
        // handing off the child. Reserve the same nine-slot production profile.
        out.owner_prefix_reads = ext4_directory_create_owner_buffers;
        out.suffix_reads = origin == Ext4MountedTypedOrigin::startup ? bound.startup_suffix : bound.mutation_suffix;
        u64 base{}, coefficient{}, scaled{};
        switch (route)
        {
        case Ext4MountedTypedRoute::zero_fresh:
            base = 5849; coefficient = 2124; out.writes = 13; out.flushes = 6; out.sequence_delta = 2; break;
        case Ext4MountedTypedRoute::zero_recovery:
            base = 4351; coefficient = 1593; out.writes = 5; out.flushes = 3; out.sequence_delta = 1; break;
        case Ext4MountedTypedRoute::zero_noop:
            base = 1446; coefficient = 531; out.suffix_reads = 1; break;
        case Ext4MountedTypedRoute::allocated_fresh:
            base = 5884; coefficient = 2125; out.writes = 14; out.flushes = 7; out.sequence_delta = 2; break;
        case Ext4MountedTypedRoute::allocated_recovery:
            base = 4366; coefficient = 1593; out.writes = 5; out.flushes = 3; out.sequence_delta = 1; break;
        }
        if (!multiply(coefficient, groups, scaled) || !add(base, scaled, out.child_reads) ||
            !add(out.classifier_reads, out.owner_prefix_reads, out.reads) ||
            !add(out.reads, out.child_reads, out.reads) || !add(out.reads, out.suffix_reads, out.reads))
            return Status::overflow("mounted typed whole route budget overflow");
        if (auto status = counters<Counter>(initial, out); !status) return status;
        return out;
    }

    // Startup learns sequence at head READ4. This pure refinement follows the
    // counter reservation; a caller must authenticate it before writer admission.
    [[nodiscard]] static Result<u32> final_sequence(Ext4MountedTypedRoute route, u32 sequence)
    {
        if (!sequence) return Status::invalid_argument("mounted typed zero journal sequence");
        switch (route)
        {
        case Ext4MountedTypedRoute::zero_noop: return sequence;
        case Ext4MountedTypedRoute::zero_fresh:
        case Ext4MountedTypedRoute::allocated_fresh:
            if (sequence > 0xfffffffcU) return Status::overflow("mounted typed fresh sequence exhausted");
            return sequence + 2;
        case Ext4MountedTypedRoute::zero_recovery:
        case Ext4MountedTypedRoute::allocated_recovery:
            if (sequence < 2 || sequence > 0xfffffffdU)
                return Status::overflow("mounted typed recovery sequence exhausted");
            return sequence + 1;
        }
        return Status::invalid_argument("mounted typed sequence route");
    }
  private:
    [[nodiscard]] static bool add(u64 first, u64 second, u64 &out)
    {
        if (second > ~u64{0} - first) return false;
        out = first + second; return true;
    }
    [[nodiscard]] static bool multiply(u64 first, u64 second, u64 &out)
    {
        if (first && second > ~u64{0} / first) return false;
        out = first * second; return true;
    }
};
} // namespace ok::fs
