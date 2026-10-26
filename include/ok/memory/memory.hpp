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

inline constexpr usize max_virtual_mappings = 256;
inline constexpr usize vm_test_page_size = 4096;
inline constexpr usize max_user_pages = 32;

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

struct PageTableEntry
{
    uptr virtual_address{0};
    PhysicalFrame frame{};
    usize permissions{0};
    bool present{false};
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

enum class PageFaultKind : u8
{
    not_present,
    protection,
    user_access,
    write_to_read_only,
    execute_disabled,
};

struct CopyResult
{
    usize bytes{0};
    Status status{Status::success()};

    [[nodiscard]] bool ok() const
    {
        return status.ok();
    }
};

template <typename T> struct UserPtr
{
    uptr address{0};

    [[nodiscard]] constexpr bool null() const
    {
        return address == 0;
    }
};

template <typename T> struct UserSlice
{
    uptr address{0};
    usize count{0};

    [[nodiscard]] constexpr usize byte_size() const
    {
        return count * sizeof(T);
    }
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

class PageTable final
{
  public:
    Status map(uptr virtual_address, PhysicalFrame frame, usize permissions);
    Status unmap(uptr virtual_address);
    [[nodiscard]] PageTableEntry *lookup(uptr virtual_address);
    [[nodiscard]] const PageTableEntry *lookup(uptr virtual_address) const;
    [[nodiscard]] usize mapping_count() const
    {
        return entries_.size();
    }
    [[nodiscard]] StaticVector<PageTableEntry, max_virtual_mappings> &entries()
    {
        return entries_;
    }
    [[nodiscard]] const StaticVector<PageTableEntry, max_virtual_mappings> &entries() const
    {
        return entries_;
    }

  private:
    StaticVector<PageTableEntry, max_virtual_mappings> entries_;
};

class KernelAddressSpace final
{
  public:
    Status map_page(uptr virtual_address, PhysicalFrame frame, usize permissions);
    Status unmap_page(uptr virtual_address);
    [[nodiscard]] const PageTableEntry *lookup(uptr virtual_address) const
    {
        return table_.lookup(virtual_address);
    }
    [[nodiscard]] usize mapping_count() const
    {
        return table_.mapping_count();
    }

  private:
    PageTable table_;
};

class UserAddressSpace final
{
  public:
    Status map_page(uptr virtual_address, PhysicalFrame frame, usize permissions);
    Status unmap_page(uptr virtual_address);
    Status clone_metadata_from(const UserAddressSpace &source);
    Status mark_copy_on_write(uptr virtual_address);
    [[nodiscard]] const PageTableEntry *lookup(uptr virtual_address) const
    {
        return table_.lookup(virtual_address);
    }
    [[nodiscard]] bool valid(UserSlice<const std::byte> slice, usize required_permissions) const;
    CopyResult copy_from_user(UserSlice<const std::byte> source, std::span<std::byte> destination) const;
    CopyResult copy_to_user(UserSlice<std::byte> destination, std::span<const std::byte> source);
    Result<FixedString<256>> copy_c_string_from_user(UserPtr<const char> source, usize max_length) const;

    template <typename T> CopyResult copy_vector_from_user(UserSlice<const T> source, std::span<T> destination) const
    {
        const usize requested = source.count < destination.size() ? source.count : destination.size();
        return copy_from_user(UserSlice<const std::byte>{.address = source.address, .count = requested * sizeof(T)},
                              std::span<std::byte>{reinterpret_cast<std::byte *>(destination.data()),
                                                   requested * sizeof(T)});
    }

  private:
    struct UserPage
    {
        bool used{false};
        uptr virtual_address{0};
        usize permissions{0};
        std::array<std::byte, vm_test_page_size> data{};
    };

    [[nodiscard]] UserPage *page_for(uptr address);
    [[nodiscard]] const UserPage *page_for(uptr address) const;
    [[nodiscard]] Status validate_range(uptr address, usize size, usize required_permissions) const;

    PageTable table_;
    std::array<UserPage, max_user_pages> pages_{};
};

class VirtualMemoryManager final
{
  public:
    Status initialize(FrameAllocator &frames);
    Result<PhysicalFrame> allocate_user_frame();
    [[nodiscard]] KernelAddressSpace &kernel_space()
    {
        return kernel_space_;
    }
    [[nodiscard]] UserAddressSpace &user_space()
    {
        return user_space_;
    }

  private:
    FrameAllocator *frames_{nullptr};
    KernelAddressSpace kernel_space_;
    UserAddressSpace user_space_;
};

[[nodiscard]] PageFaultKind classify_page_fault(bool present, bool write, bool user, bool execute,
                                                usize permissions);

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
    [[nodiscard]] LinearAddressSpace &kernel_address_space()
    {
        return kernel_space_;
    }

  private:
    TranslationMode mode_{TranslationMode::linear};
    FrameAllocator frames_;
    MemoryRegion metadata_region_{};
    SlabAllocator objects_;
    LinearAddressSpace kernel_space_;
};



} // namespace ok::memory
