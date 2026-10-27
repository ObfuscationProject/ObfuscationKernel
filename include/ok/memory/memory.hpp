#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/memory/frames.hpp"
#include "ok/memory/slab.hpp"

#include <array>
#include <span>

namespace ok::memory
{

enum class TranslationMode : u8
{
    linear,
    paged,
    higher_half,
};

class PhysicalMemoryMap;

inline constexpr usize vm_test_page_size = 4096;

enum PagePermission : usize
{
    page_read = 1u << 0,
    page_write = 1u << 1,
    page_execute = 1u << 2,
    page_user = 1u << 3,
    page_global = 1u << 4,
    page_device = 1u << 5,
    page_no_cache = 1u << 6,
    page_copy_on_write = 1u << 7,
    page_shared = 1u << 8,
};

struct VmArea
{
    uptr base{0};
    usize length{0};
    usize permissions{0};

    [[nodiscard]] constexpr bool contains(uptr address, usize size = 1) const
    {
        return address >= base && size <= length && address - base <= length - size;
    }
};

class MemoryManager final
{
  public:
    void set_translation_mode(TranslationMode mode)
    {
        mode_ = mode;
    }
    [[nodiscard]] TranslationMode translation_mode() const
    {
        return mode_;
    }
    // Boot form: carves the allocator metadata from the top of the highest
    // usable range of a normalized map and manages every remaining usable page.
    // The map must hold only RAM the supervisor can already access. Frames carry
    // physical + `direct_offset` (see PhysicalFrame).
    Status initialize(const PhysicalMemoryMap &map, usize page_size, uptr direct_offset = 0);
    // Model form: the caller owns the metadata storage.
    Status initialize(std::span<const MemoryRegion> regions, usize page_size, std::span<std::byte> metadata);
    [[nodiscard]] FrameAllocator &frames()
    {
        return frames_;
    }
    [[nodiscard]] const FrameAllocator &frames() const
    {
        return frames_;
    }
    // Carved metadata in frame (direct-map) addresses; empty for the model form.
    [[nodiscard]] MemoryRegion metadata_region() const
    {
        return metadata_region_;
    }
    // Kernel objects over the managed frames. Only after the boot form: model
    // frames are bookkeeping that is never dereferenced.
    Status initialize_objects();
    [[nodiscard]] SlabAllocator &objects()
    {
        return objects_;
    }

  private:
    TranslationMode mode_{TranslationMode::linear};
    FrameAllocator frames_;
    MemoryRegion metadata_region_{};
    SlabAllocator objects_;
};

} // namespace ok::memory
