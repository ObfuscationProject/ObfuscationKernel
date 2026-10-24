#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/memory/charges.hpp"
#include "ok/memory/physical_map.hpp"

#include <span>

namespace ok::memory
{

struct RuntimeFrame
{
    uptr address{0};
    usize index{0};
    u64 owner{0};
    u64 generation{0};
};

// Frame allocator whose metadata is sized at initialisation from a normalised
// PhysicalMemoryMap instead of a compile-time page count. The caller supplies
// the metadata storage (`metadata_bytes_required()`); in production that is a
// region carved from discovered RAM before any other allocation, in hosted
// tests an array. Generation tracking per frame rejects stale and foreign
// handles like the bootstrap FrameAllocator. Single serialised caller only; IRQ
// and SMP synchronisation arrive with feat/irq-wait-time and feat/smp-bringup.
class RuntimeFrameAllocator final : private NonCopyable
{
  public:
    [[nodiscard]] static usize metadata_bytes_required(const PhysicalMemoryMap &map, usize page_size);

    Status initialize(const PhysicalMemoryMap &map, usize page_size, std::span<std::byte> metadata);
    Result<RuntimeFrame> allocate(ChargeAccount &account);
    Status release(RuntimeFrame frame, ChargeAccount &account);

    [[nodiscard]] bool initialized() const
    {
        return frame_count_ != 0;
    }
    [[nodiscard]] usize page_size() const
    {
        return page_size_;
    }
    [[nodiscard]] usize total_frames() const
    {
        return frame_count_;
    }
    [[nodiscard]] usize free_frames() const
    {
        return frame_count_ - used_count_;
    }
    [[nodiscard]] u64 identity() const
    {
        return identity_;
    }

  private:
    struct Range
    {
        uptr base{0};
        usize first_index{0};
        usize count{0};
    };

    [[nodiscard]] bool bit(usize index) const;
    void set_bit(usize index, bool value);
    [[nodiscard]] bool address_for(usize index, uptr &address) const;

    usize page_size_{0};
    usize frame_count_{0};
    usize used_count_{0};
    usize next_free_hint_{0};
    usize range_count_{0};
    u64 identity_{0};
    u64 next_generation_{1};
    std::span<u64> bitmap_{};
    std::span<u64> generations_{};
    std::span<Range> ranges_{};
};

} // namespace ok::memory
