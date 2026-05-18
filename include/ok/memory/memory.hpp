#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"

#include <span>

namespace ok::memory
{

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

struct PhysicalFrame
{
    uptr address{0};
    usize index{0};
};

inline constexpr usize max_physical_frames = 16384;
inline constexpr usize max_virtual_mappings = 256;

class FrameAllocator final
{
  public:
    Status initialize(std::span<const MemoryRegion> regions, usize page_size);
    Result<PhysicalFrame> allocate();
    Status release(PhysicalFrame frame);

    [[nodiscard]] usize page_size() const
    {
        return page_size_;
    }
    [[nodiscard]] usize total_frames() const
    {
        return frame_used_.size();
    }
    [[nodiscard]] usize free_frames() const;

  private:
    uptr base_{0};
    usize page_size_{4096};
    usize frame_count_{0};
    std::array<bool, max_physical_frames> frame_used_{};
};

class AddressSpace
{
  public:
    virtual ~AddressSpace() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Status map(uptr virtual_address, PhysicalFrame frame, usize flags) = 0;
    virtual Status unmap(uptr virtual_address) = 0;
};

class LinearAddressSpace final : public AddressSpace
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "linear-address-space";
    }
    Status map(uptr virtual_address, PhysicalFrame frame, usize flags) override;
    Status unmap(uptr virtual_address) override;
    [[nodiscard]] usize mapping_count() const
    {
        return mappings_.size();
    }

  private:
    struct Mapping
    {
        uptr virtual_address;
        PhysicalFrame frame;
        usize flags;
    };

    StaticVector<Mapping, max_virtual_mappings> mappings_;
};

class MemoryManager final
{
  public:
    Status initialize(std::span<const MemoryRegion> regions, usize page_size);
    [[nodiscard]] FrameAllocator &frames()
    {
        return frames_;
    }
    [[nodiscard]] const FrameAllocator &frames() const
    {
        return frames_;
    }
    [[nodiscard]] LinearAddressSpace &kernel_address_space()
    {
        return kernel_space_;
    }

  private:
    FrameAllocator frames_;
    LinearAddressSpace kernel_space_;
};

} // namespace ok::memory
