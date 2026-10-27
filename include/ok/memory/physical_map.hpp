#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/memory/memory.hpp"

#include <array>
#include <span>

namespace ok::memory
{

// Where a region description came from. Product boots must not use `synthetic`.
enum class MapSource : u8
{
    synthetic,
    e820,
    fdt,
    board_table,
    linker,
};

struct DiscoveredRegion
{
    MemoryRegion region{};
    MapSource source{MapSource::synthetic};
};

inline constexpr usize max_discovered_regions = 64;

// Normalised view of discovered physical memory: the usable set minus every
// reserved, device and kernel-image byte. Regions are added in any order and
// may overlap; `normalize()` produces sorted, disjoint, page-aligned usable
// ranges. Reservations always win over usable declarations. The map itself has
// fixed metadata; the frame allocator built on it sizes its own metadata from
// the result (see MemoryManager::initialize).
class PhysicalMemoryMap final : private NonCopyable
{
  public:
    void reset()
    {
        input_count_ = usable_count_ = 0;
        normalized_ = false;
    }
    Status add(MemoryRegion region, MapSource source);
    Status reserve(uptr base, usize size, RegionType type = RegionType::reserved);
    // Produces the usable ranges. Fails with overflow when more than
    // max_discovered_regions disjoint usable ranges would result.
    Status normalize(usize page_size);

    [[nodiscard]] std::span<const MemoryRegion> usable() const
    {
        return {usable_.data(), usable_count_};
    }
    [[nodiscard]] std::span<const DiscoveredRegion> inputs() const
    {
        return {inputs_.data(), input_count_};
    }
    [[nodiscard]] usize usable_bytes() const;
    [[nodiscard]] usize usable_pages(usize page_size) const;
    [[nodiscard]] bool normalized() const
    {
        return normalized_;
    }
    [[nodiscard]] bool product_grade() const;

  private:
    std::array<DiscoveredRegion, max_discovered_regions> inputs_{};
    std::array<MemoryRegion, max_discovered_regions> usable_{};
    usize input_count_{0};
    usize usable_count_{0};
    bool normalized_{false};
};

} // namespace ok::memory
