#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/memory/charges.hpp"

#include <array>
#include <span>

namespace ok::memory
{

// Region classification shared by memory maps and the frame allocator.
enum class RegionType : u8
{
    usable,
    reserved,
    device_mmio,
    kernel_image,
};

struct MemoryRegion
{
    uptr base{0};
    usize size{0};
    RegionType type{RegionType::reserved};
};

class FrameAllocator;

// `address` is the frame's supervisor direct-map address. It equals the bus
// physical address except on profiles whose kernel runs in a fixed segment
// (MIPS KSEG0), where it is the established segment alias.
struct PhysicalFrame
{
    uptr address{0};
    usize index{0};
    const FrameAllocator *owner{nullptr};
    u64 generation{0};
};

// Per-frame state kept in allocator metadata. `generation` is zero while the
// frame is free. The other fields belong to the frame's current owner and are
// cleared on every allocation; the allocator never interprets them.
struct PageDescriptor
{
    u64 generation{0};
    u32 references{0};
    u16 pins{0};
    u8 kind{0};
    u8 flags{0};
};
static_assert(sizeof(PageDescriptor) == 16);

// PageDescriptor::kind values of the owners that share one allocator. An owner
// only interprets descriptors of its own kind.
enum class PageKind : u8
{
    unowned = 0,
    native_user = 1,
    native_object = 2,
    kernel_slab = 3,
    kernel_large = 4,
    page_cache = 5,
};

// Physical frame ownership over runtime-sized metadata. The caller supplies the
// metadata storage: at boot a range carved from discovered RAM, in models a
// static array. Every check precedes the first metadata write, so a failed
// initialization leaves the previous allocator intact. Single serialized
// caller only; IRQ and SMP synchronization arrive with later worktrees.
class FrameAllocator final : private NonCopyable
{
  public:
    struct FrameRange
    {
        uptr base{0};
        usize first_index{0};
        usize count{0};
    };

    // Storage for `frames` descriptors in `ranges` ranges, with alignment slack.
    [[nodiscard]] static constexpr usize metadata_bytes_for(usize frames, usize ranges)
    {
        return (frames + 63) / 64 * sizeof(u64) + frames * sizeof(PageDescriptor) + ranges * sizeof(FrameRange) +
               metadata_alignment - 1;
    }
    static constexpr usize metadata_alignment = 8;
    // Exact storage requirement of a region set; fails like initialize() would.
    [[nodiscard]] static Result<usize> metadata_bytes(std::span<const MemoryRegion> regions, usize page_size);

    Status initialize(std::span<const MemoryRegion> regions, usize page_size, std::span<std::byte> metadata);
    Result<PhysicalFrame> allocate();
    Result<PhysicalFrame> allocate(ChargeAccount &account);
    // Physically contiguous frames inside one range. Returns the lowest; the
    // others are the next indices (handle_at). Release them together.
    Result<PhysicalFrame> allocate_contiguous(usize count, ChargeAccount &account);
    Status release_contiguous(PhysicalFrame first, usize count, ChargeAccount &account);
    Status release(PhysicalFrame frame);
    Status release(PhysicalFrame frame, ChargeAccount &account);

    // Descriptor of a live exact handle; null for a foreign, stale or free handle.
    [[nodiscard]] PageDescriptor *descriptor(const PhysicalFrame &frame);
    [[nodiscard]] const PageDescriptor *descriptor(const PhysicalFrame &frame) const;
    [[nodiscard]] bool live(const PhysicalFrame &frame) const
    {
        return descriptor(frame) != nullptr;
    }
    // The current handle of an allocated frame.
    [[nodiscard]] Result<PhysicalFrame> handle_at(usize index) const;
    [[nodiscard]] Result<usize> index_of(uptr address) const;

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
    [[nodiscard]] usize used_frames() const
    {
        return used_count_;
    }
    [[nodiscard]] std::span<const FrameRange> ranges() const
    {
        return ranges_;
    }

  private:
    [[nodiscard]] bool used(usize index) const;
    void mark(usize index, bool value);
    [[nodiscard]] uptr address_for(usize index) const;
    [[nodiscard]] bool exact(const PhysicalFrame &frame) const;
    PhysicalFrame take(usize index);

    usize page_size_{4096};
    usize frame_count_{0};
    usize used_count_{0};
    usize next_free_hint_{0};
    u64 next_generation_{1};
    std::span<u64> bitmap_{};
    std::span<PageDescriptor> descriptors_{};
    std::span<FrameRange> ranges_{};
};

// Caller-owned metadata storage for a region set with a known bound.
template <usize Frames, usize Ranges = 1> struct FrameMetadataStorage
{
    alignas(FrameAllocator::metadata_alignment) std::array<std::byte, FrameAllocator::metadata_bytes_for(Frames, Ranges)> bytes{};
};

} // namespace ok::memory
