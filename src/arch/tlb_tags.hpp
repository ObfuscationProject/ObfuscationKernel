#pragma once

#include "ok/core/types.hpp"

#include <array>

// Tagged user TLB entries (aarch64 ASIDs, rv64 satp ASIDs, x86_64 PCIDs).
// Each CPU picks its own tags: a space's tag differs between CPUs, so no tag
// is ever shared and no cross-CPU allocator or rollover handshake exists. An
// entry revalidates what this CPU cached against the space's TLB generation,
// which covers CPUs that ran the space earlier and left; quiesce_root still
// covers CPUs inside a user window. Hardware never uses entries of a tag that
// is not current, and a tag becomes current for another space only after it
// was invalidated, so a retired space's leftover entries are unreachable.
namespace ok::arch::detail
{

// What an entry needs to know about its space. `id` comes from one sequence
// and never repeats, so a cached tag never matches a later space, even one
// whose root reuses the same frame. `generation` moves after each change of
// an existing translation.
struct TlbSpaceTag
{
    u64 id{};
    u64 generation{};

    // Called after a translation was rewritten and every CPU that may run the
    // space left its user window (quiesce_root). Bumping before the rewrite
    // would let a CPU still in its window cache the old translation under the
    // new generation.
    constexpr void changed() noexcept { ++generation; }
};

// Issues space ids. Serialized by the backend's space lock.
class TlbSpaceIds
{
  public:
    [[nodiscard]] constexpr TlbSpaceTag issue() noexcept { return {next_++, 0}; }

  private:
    u64 next_{1};
};

// The tag one entry runs under and whether that tag's entries must be
// invalidated on this CPU before user code runs.
struct TlbSelection
{
    u32 tag{};
    bool invalidate{};
};

// One CPU's map from spaces to the tags it uses for them. Tag 0 stays the
// kernel's translation. Only its own CPU touches it, with interrupts masked.
// Once every usable tag is assigned, the next new space starts a new round
// (a rollover): tags are handed out again from the first, and each one is
// invalidated as it is reassigned.
template <usize Slots> class TlbTagCache
{
    static_assert(Slots > 0 && Slots < 0xffff);

  public:
    // `bits` is the hardware tag width; 0 leaves this CPU untagged, which
    // keeps a full user TLB flush on every entry.
    constexpr void configure(u32 bits) noexcept
    {
        *this = {};
        bits_ = bits > 16 ? 16 : bits;
        const u64 usable = bits_ == 0 ? 0 : (u64{1} << bits_) - 1;
        limit_ = static_cast<u32>(usable < Slots ? usable : Slots);
    }
    [[nodiscard]] constexpr bool tagged() const noexcept { return limit_ != 0; }
    [[nodiscard]] constexpr u32 bits() const noexcept { return bits_; }
    [[nodiscard]] constexpr u32 limit() const noexcept { return limit_; }
    [[nodiscard]] constexpr u64 rollovers() const noexcept { return rollovers_; }
    [[nodiscard]] constexpr u64 invalidations() const noexcept { return invalidations_; }

    // Only for a tagged CPU, right before it enters `space`.
    [[nodiscard]] constexpr TlbSelection select(const TlbSpaceTag &space) noexcept
    {
        for (u32 index = 0; index < used_; ++index)
            if (slots_[index].id == space.id)
            {
                const bool stale = slots_[index].generation != space.generation;
                slots_[index].generation = space.generation;
                invalidations_ += stale;
                return {index + 1, stale};
            }
        if (used_ == limit_)
        {
            used_ = 0;
            ++rollovers_;
        }
        slots_[used_] = {space.id, space.generation};
        ++invalidations_;
        return {++used_, true};
    }

  private:
    struct Slot
    {
        u64 id{};
        u64 generation{};
    };
    std::array<Slot, Slots> slots_{};
    u32 used_{}, limit_{}, bits_{};
    u64 rollovers_{}, invalidations_{};
};

// Per-CPU tag capacity of the kernel backends.
inline constexpr usize tlb_tag_slots = 32;
using CpuTlbTags = TlbTagCache<tlb_tag_slots>;

} // namespace ok::arch::detail

namespace ok::arch::execution
{

// The user TLB mode of the CPUs in a mask, for the OK_SMP_TLB line: the
// narrowest tag width among them (0: untagged), whether every one of them
// tags with x86 PCIDs, and their tag rollovers so far.
struct TlbTagReport
{
    bool supported{};
    u32 asid_bits{};
    bool pcid{};
    u64 rollovers{};
};
// x86_64, aarch64 and rv64 define it; profiles without secondary CPUs report
// unsupported (src/arch/smp_stub.cpp).
[[nodiscard]] TlbTagReport tlb_tag_report(u32 online_mask) noexcept;

// Folds the caches of the first `cpus` CPUs that `mask` names; `cache_of(cpu)`
// returns one. `pcid`: tagged CPUs of this backend tag with x86 PCIDs.
template <typename CacheOf>
[[nodiscard]] constexpr TlbTagReport summarize_tlb_tags(u32 mask, usize cpus, bool pcid, CacheOf cache_of) noexcept
{
    TlbTagReport report{.supported = true, .asid_bits = 16};
    for (u32 cpu = 0; cpu < cpus && cpu < 32; ++cpu)
        if ((mask & (u32{1} << cpu)) != 0)
        {
            const auto &cache = cache_of(cpu);
            report.asid_bits = cache.bits() < report.asid_bits ? cache.bits() : report.asid_bits;
            report.rollovers += cache.rollovers();
        }
    report.pcid = pcid && report.asid_bits != 0;
    return report;
}

} // namespace ok::arch::execution
