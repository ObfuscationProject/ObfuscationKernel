#pragma once

#include "ok/memory/frames.hpp"

#include <array>

namespace ok::memory
{

// Who is allocating. Nothing may block yet. Only `atomic` (future interrupt top
// halves and reclaim paths) may take frames from the reserve.
enum class AllocationContext : u8
{
    boot,
    kernel,
    atomic,
};

inline constexpr usize allocation_context_count = 3;

struct SlabClassAccounting
{
    usize object_size{};
    usize slots_per_page{};
    usize pages{};
    usize live_objects{};
};

struct SlabAccounting
{
    std::array<SlabClassAccounting, 7> classes{};
    usize large_allocations{};
    usize large_pages{};
    usize pages{};      // All frames charged to the allocator.
    usize peak_pages{};
    std::array<u64, allocation_context_count> allocations{};
    u64 releases{};
    u64 failures{};
};

// General kernel object allocator. Objects up to 1024 bytes use power-of-two
// size classes in slab pages; larger objects take physically contiguous frames.
//
// Owner: the MemoryManager that initializes it. Accounting: every frame it holds
// is charged to its own page account (with a limit); every object charges its
// caller's account in bytes of its class or pages. Allocation context: a single
// serialized boot-CPU caller; `atomic` may use the frame reserve that `boot` and
// `kernel` must leave free. Reclamation: a slab page whose last object is
// released returns to the frame allocator unless it is its class's only
// partially used page; large objects return their frames on release.
// Objects are aligned to their class size (large ones to a page). Releasing
// checks the page kind, class, slot alignment and slot occupancy, so foreign,
// misaligned and double releases fail without changing any state.
class SlabAllocator final : private NonCopyable
{
  public:
    static constexpr std::array<usize, 7> size_classes{16, 32, 64, 128, 256, 512, 1024};
    static constexpr usize large_threshold = size_classes.back();

    Status initialize(FrameAllocator &frames, usize page_limit, usize reserve_frames);
    Result<void *> allocate(usize size, ChargeAccount &owner, AllocationContext context = AllocationContext::kernel);
    Status release(void *object, usize size, ChargeAccount &owner);

    [[nodiscard]] bool initialized() const
    {
        return frames_ != nullptr;
    }
    [[nodiscard]] usize reserve_frames() const
    {
        return reserve_;
    }
    [[nodiscard]] const ChargeAccount &page_account() const
    {
        return pages_;
    }
    [[nodiscard]] SlabAccounting accounting() const;
    // Bytes an allocation of `size` charges to its owner; zero when unsupported.
    [[nodiscard]] usize charge_for(usize size) const;

  private:
    struct Class
    {
        uptr partial{}; // Head of the doubly linked list of pages with free slots.
        usize pages{};
        usize live{};
    };

    [[nodiscard]] static usize class_index(usize size);
    Result<uptr> new_page(usize index, AllocationContext context);
    Status reserve_frames_for(usize count, AllocationContext context) const;
    Result<PhysicalFrame> owned_page(uptr address, PageKind kind) const;
    void unlink(usize index, uptr page);
    void push(usize index, uptr page);
    Result<void *> allocate_large(usize size, ChargeAccount &owner, AllocationContext context);
    Status release_large(void *object, usize size, ChargeAccount &owner);

    FrameAllocator *frames_{};
    usize reserve_{};
    ChargeAccount pages_{};
    std::array<Class, size_classes.size()> classes_{};
    usize large_allocations_{};
    usize large_pages_{};
    std::array<u64, allocation_context_count> allocations_{};
    u64 releases_{};
    u64 failures_{};
};

} // namespace ok::memory
