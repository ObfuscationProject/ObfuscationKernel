#pragma once

// Software VM model for the kernel's roadmap tests and test points. It keeps
// mappings and user page contents in fixed in-object tables and never touches
// hardware translation; native user memory lives in ok/user and ok/arch.
#include "ok/core/fixed.hpp"
#include "ok/memory/memory.hpp"

#include <array>
#include <span>

namespace ok::memory::model
{

inline constexpr usize max_virtual_mappings = 256;
inline constexpr usize max_user_pages = 32;

struct PageTableEntry
{
    uptr virtual_address{0};
    PhysicalFrame frame{};
    usize permissions{0};
    bool present{false};
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

inline Status LinearAddressSpace::map(uptr virtual_address, PhysicalFrame frame, usize flags)
{
    for (const auto &mapping : mappings_)
    {
        if (mapping.virtual_address == virtual_address)
        {
            return Status::already_exists("virtual address already mapped");
        }
    }
    return mappings_.push_back(Mapping{.virtual_address = virtual_address, .frame = frame, .flags = flags});
}

inline Status LinearAddressSpace::unmap(uptr virtual_address)
{
    for (usize i = 0; i < mappings_.size(); ++i)
    {
        if (mappings_[i].virtual_address == virtual_address)
        {
            return mappings_.erase_at(i);
        }
    }
    return Status::not_found("virtual address not mapped");
}

namespace detail
{

inline uptr page_base(uptr address)
{
    return address / vm_test_page_size * vm_test_page_size;
}

inline usize page_offset(uptr address)
{
    return static_cast<usize>(address - page_base(address));
}

} // namespace detail

inline Status PageTable::map(uptr virtual_address, PhysicalFrame frame, usize permissions)
{
    if ((virtual_address % vm_test_page_size) != 0)
    {
        return Status::invalid_argument("virtual address is not page aligned");
    }
    for (const auto &entry : entries_)
    {
        if (entry.present && entry.virtual_address == virtual_address)
        {
            return Status::already_exists("page is already mapped");
        }
    }
    return entries_.push_back(PageTableEntry{
        .virtual_address = virtual_address,
        .frame = frame,
        .permissions = permissions,
        .present = true,
    });
}

inline Status PageTable::unmap(uptr virtual_address)
{
    const auto base = detail::page_base(virtual_address);
    for (usize i = 0; i < entries_.size(); ++i)
    {
        if (entries_[i].present && entries_[i].virtual_address == base)
        {
            return entries_.erase_at(i);
        }
    }
    return Status::not_found("page is not mapped");
}

inline PageTableEntry *PageTable::lookup(uptr virtual_address)
{
    const auto base = detail::page_base(virtual_address);
    for (auto &entry : entries_)
    {
        if (entry.present && entry.virtual_address == base)
        {
            return &entry;
        }
    }
    return nullptr;
}

inline const PageTableEntry *PageTable::lookup(uptr virtual_address) const
{
    const auto base = detail::page_base(virtual_address);
    for (const auto &entry : entries_)
    {
        if (entry.present && entry.virtual_address == base)
        {
            return &entry;
        }
    }
    return nullptr;
}

inline Status KernelAddressSpace::map_page(uptr virtual_address, PhysicalFrame frame, usize permissions)
{
    return table_.map(virtual_address, frame, permissions | page_global);
}

inline Status KernelAddressSpace::unmap_page(uptr virtual_address)
{
    return table_.unmap(virtual_address);
}

inline UserAddressSpace::UserPage *UserAddressSpace::page_for(uptr address)
{
    const auto base = detail::page_base(address);
    for (auto &page : pages_)
    {
        if (page.used && page.virtual_address == base)
        {
            return &page;
        }
    }
    return nullptr;
}

inline const UserAddressSpace::UserPage *UserAddressSpace::page_for(uptr address) const
{
    const auto base = detail::page_base(address);
    for (const auto &page : pages_)
    {
        if (page.used && page.virtual_address == base)
        {
            return &page;
        }
    }
    return nullptr;
}

inline Status UserAddressSpace::map_page(uptr virtual_address, PhysicalFrame frame, usize permissions)
{
    if ((permissions & page_user) == 0)
    {
        return Status::invalid_argument("user page mapping requires user permission");
    }
    if (auto status = table_.map(virtual_address, frame, permissions); !status.ok())
    {
        return status;
    }
    for (auto &page : pages_)
    {
        if (!page.used)
        {
            page = UserPage{.used = true, .virtual_address = virtual_address, .permissions = permissions};
            return Status::success();
        }
    }
    static_cast<void>(table_.unmap(virtual_address));
    return Status::overflow("user page table capacity exceeded");
}

inline Status UserAddressSpace::unmap_page(uptr virtual_address)
{
    const auto base = detail::page_base(virtual_address);
    for (auto &page : pages_)
    {
        if (page.used && page.virtual_address == base)
        {
            page = {};
            return table_.unmap(base);
        }
    }
    return Status::not_found("user page is not mapped");
}

inline Status UserAddressSpace::clone_metadata_from(const UserAddressSpace &source)
{
    table_ = {};
    pages_ = {};
    for (const auto &entry : source.table_.entries())
    {
        if (!entry.present)
        {
            continue;
        }
        if (auto status = map_page(entry.virtual_address, entry.frame, entry.permissions); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

inline Status UserAddressSpace::mark_copy_on_write(uptr virtual_address)
{
    auto *entry = table_.lookup(virtual_address);
    auto *page = page_for(virtual_address);
    if (entry == nullptr || page == nullptr)
    {
        return Status::not_found("copy-on-write page is not mapped");
    }
    entry->permissions = (entry->permissions & ~page_write) | page_copy_on_write;
    page->permissions = entry->permissions;
    return Status::success();
}

inline Status UserAddressSpace::validate_range(uptr address, usize size, usize required_permissions) const
{
    if (size == 0)
    {
        return Status::success();
    }
    if (address == 0)
    {
        return Status::invalid_argument("user pointer is null");
    }
    for (usize cursor = 0; cursor < size;)
    {
        const auto current = address + cursor;
        const auto *page = page_for(current);
        if (page == nullptr)
        {
            return Status::fault("user pointer is unmapped");
        }
        if ((page->permissions & page_user) == 0 || (page->permissions & required_permissions) != required_permissions)
        {
            return Status::denied("user pointer permissions do not allow access");
        }
        const usize available = vm_test_page_size - detail::page_offset(current);
        const usize step = (size - cursor) < available ? (size - cursor) : available;
        cursor += step;
    }
    return Status::success();
}

inline bool UserAddressSpace::valid(UserSlice<const std::byte> slice, usize required_permissions) const
{
    return validate_range(slice.address, slice.count, required_permissions).ok();
}

inline CopyResult UserAddressSpace::copy_from_user(UserSlice<const std::byte> source,
                                            std::span<std::byte> destination) const
{
    const usize count = source.count < destination.size() ? source.count : destination.size();
    if (auto status = validate_range(source.address, count, page_read); !status.ok())
    {
        return CopyResult{.bytes = 0, .status = status};
    }
    for (usize copied = 0; copied < count;)
    {
        const auto current = source.address + copied;
        const auto *page = page_for(current);
        const usize offset = detail::page_offset(current);
        const usize available = vm_test_page_size - offset;
        const usize step = (count - copied) < available ? (count - copied) : available;
        for (usize i = 0; i < step; ++i)
        {
            destination[copied + i] = page->data[offset + i];
        }
        copied += step;
    }
    return CopyResult{.bytes = count, .status = Status::success()};
}

inline CopyResult UserAddressSpace::copy_to_user(UserSlice<std::byte> destination, std::span<const std::byte> source)
{
    const usize count = destination.count < source.size() ? destination.count : source.size();
    if (auto status = validate_range(destination.address, count, page_write); !status.ok())
    {
        return CopyResult{.bytes = 0, .status = status};
    }
    for (usize copied = 0; copied < count;)
    {
        const auto current = destination.address + copied;
        auto *page = page_for(current);
        const usize offset = detail::page_offset(current);
        const usize available = vm_test_page_size - offset;
        const usize step = (count - copied) < available ? (count - copied) : available;
        for (usize i = 0; i < step; ++i)
        {
            page->data[offset + i] = source[copied + i];
        }
        copied += step;
    }
    return CopyResult{.bytes = count, .status = Status::success()};
}

inline Result<FixedString<256>> UserAddressSpace::copy_c_string_from_user(UserPtr<const char> source, usize max_length) const
{
    if (source.null())
    {
        return Status::invalid_argument("user string pointer is null");
    }
    FixedString<256> out;
    const usize limit = max_length < 255 ? max_length : 255;
    for (usize i = 0; i < limit; ++i)
    {
        const auto current = source.address + i;
        if (auto status = validate_range(current, 1, page_read); !status.ok())
        {
            return status;
        }
        const auto *page = page_for(current);
        const char value = static_cast<char>(page->data[detail::page_offset(current)]);
        if (value == '\0')
        {
            return out;
        }
        if (auto status = out.append(value); !status.ok())
        {
            return status;
        }
    }
    return Status::overflow("user string is not nul-terminated within bound");
}

inline Status VirtualMemoryManager::initialize(FrameAllocator &frames)
{
    frames_ = &frames;
    return Status::success();
}

inline Result<PhysicalFrame> VirtualMemoryManager::allocate_user_frame()
{
    if (frames_ == nullptr)
    {
        return Status::not_initialized("virtual memory manager has no frame allocator");
    }
    return frames_->allocate();
}

inline PageFaultKind classify_page_fault(bool present, bool write, bool user, bool execute, usize permissions)
{
    if (!present)
    {
        return PageFaultKind::not_present;
    }
    if (user && (permissions & page_user) == 0)
    {
        return PageFaultKind::user_access;
    }
    if (write && (permissions & page_write) == 0)
    {
        return PageFaultKind::write_to_read_only;
    }
    if (execute && (permissions & page_execute) == 0)
    {
        return PageFaultKind::execute_disabled;
    }
    return PageFaultKind::protection;
}

} // namespace ok::memory::model
