#pragma once

#include "ok/core/types.hpp"
#include "ok/arch/kernel_entry_stack.hpp"

#include <array>
#include <span>

namespace ok::arch::execution
{

inline constexpr usize page_size = 4096;
#if defined(__aarch64__) || defined(__arm__)
// QEMU virt places kernel RAM at 1 GiB; keep user mappings below it and above
// the platform's MMIO aperture. Each native backend owns its virtual layout.
inline constexpr uptr user_begin = 0x10000000;
inline constexpr uptr user_end = 0x40000000;
#else
inline constexpr uptr user_begin = 0x40000000;
inline constexpr uptr user_end = 0x80000000;
#endif
inline constexpr usize max_address_spaces = 16;

enum PageFlags : u8
{
    page_read = 1,
    page_write = 2,
    page_execute = 4,
};

// Physical pages must belong to the caller's reserved, page-aligned backing
// storage. The backend allocates page tables, never application data pages.
struct PageMapping
{
    uptr virtual_address{0};
    uptr physical_address{0};
    u8 flags{page_read};
    // Runtime bookkeeping that backends ignore: write access is intended but
    // withheld from `flags` while the private frame is shared copy-on-write.
    u8 copy_on_write{0};
};

inline constexpr usize max_native_mappings = 1024;
// A backend may retain this read-only directory while its address-space slot
// exists. The pointed-to records live in page-pool metadata blocks; moving the
// owner never moves those blocks. Zero-flag records retain backing for PROT_NONE
// but must not acquire a user translation.
struct MappingView
{
    static constexpr usize records_per_chunk = page_size / sizeof(PageMapping);
    static constexpr usize max_chunks =
        (max_native_mappings + records_per_chunk - 1) / records_per_chunk;
    std::array<std::span<const PageMapping>, max_chunks> chunks{};
    usize count{};

    constexpr MappingView() = default;
    constexpr MappingView(std::span<const PageMapping> contiguous) : count(contiguous.size())
    {
        chunks[0] = contiguous;
    }
    constexpr MappingView(const PageMapping *data, usize size) : MappingView(std::span<const PageMapping>{data, size}) {}
    template <usize N> constexpr MappingView(const std::array<PageMapping, N> &data)
        : MappingView(data.data(), N) {}
    template <usize N> constexpr MappingView(const PageMapping (&data)[N])
        : MappingView(data, N) {}
    [[nodiscard]] constexpr usize size() const { return count; }
    [[nodiscard]] constexpr bool empty() const { return count == 0; }
    // Validate the public directory before a backend indexes it. Subtracting
    // from count avoids overflow even when a caller supplies a huge span.
    [[nodiscard]] constexpr bool valid() const
    {
        if (count > max_native_mappings) return false;
        usize total = 0;
        for (const auto chunk : chunks)
        {
            if (chunk.size() && !chunk.data()) return false;
            if (chunk.size() > count - total) return false;
            total += chunk.size();
        }
        return total == count;
    }
    [[nodiscard]] constexpr const PageMapping &operator[](usize index) const
    {
        for (const auto chunk : chunks)
        {
            if (index < chunk.size()) return chunk[index];
            index -= chunk.size();
        }
        __builtin_unreachable();
    }
    struct Iterator
    {
        const MappingView *view{};
        usize index{};
        [[nodiscard]] constexpr const PageMapping &operator*() const { return (*view)[index]; }
        constexpr Iterator &operator++() { ++index; return *this; }
        [[nodiscard]] constexpr bool operator!=(Iterator other) const { return index != other.index; }
    };
    [[nodiscard]] constexpr Iterator begin() const { return {this, 0}; }
    [[nodiscard]] constexpr Iterator end() const { return {this, count}; }
};

struct AddressSpace
{
    uptr root{0};
    u32 slot{0};
    u32 generation{0};
};

// Serialized boot-CPU snapshot of backend slots, independent of process records.
// Includes allocated slots whose handles were never published to a process.
// Static kernel tables and inactive reusable bytes are excluded. These backends
// retire synchronously; runtime page/pin/quarantine owners are reported separately.
struct OwnershipReport
{
    Status failure{};
    usize address_spaces{};
    bool executing{};
};

[[nodiscard]] OwnershipReport ownership_report() noexcept;

// Register slots are architecture-private; use syscall()/set_return_value().
// A complete copy preserves a suspended user thread, including FP/SIMD and TLS.
// AVX/SVE and hardware SMP execution are not enabled by these native backends.
struct alignas(16) UserFrame
{
    std::array<uptr, 32> registers{};
    uptr pc{0};
    uptr sp{0};
    uptr flags{0};
    uptr fs_base{0};
    uptr gs_base{0};
    alignas(16) std::array<u8, 512> fp_state{};
};

struct Syscall
{
    uptr number{0};
    std::array<uptr, 6> arguments{};
};

enum class TrapKind : u8
{
    syscall,
    timer,
    page_fault,
    exception,
};

struct Trap
{
    TrapKind kind{TrapKind::exception};
    u32 vector{0};
    uptr error_code{0};
    uptr fault_address{0};
    uptr kernel_stack_pointer{0};
};

[[nodiscard]] bool supported() noexcept;
Status initialize();
Result<AddressSpace> create_space(MappingView mappings);
Status destroy_space(AddressSpace &space);
// Re-synchronizes one present translation with its record after the runtime
// changed that record in place (physical frame and/or flags). The handle stays
// valid; only accessible records with an existing translation qualify. Not
// while the space executes.
Status remap(const AddressSpace &space, const PageMapping &mapping);

// Deferred write access. A runtime that withholds write permission (copy-on-
// write) installs a resolver; user_range() offers it each present, readable,
// non-writable page of a kernel write once. A resolver that returns true has
// made the page writable in place through remap().
using WriteResolver = bool (*)(const AddressSpace &space, uptr page);
inline constinit WriteResolver write_resolver{};
inline void set_write_resolver(WriteResolver resolver) noexcept
{
    write_resolver = resolver;
}
[[nodiscard]] inline bool resolve_write(const AddressSpace &space, uptr page)
{
    return write_resolver != nullptr && write_resolver(space, page);
}

// Execute at user privilege until a real syscall, hardware timer interrupt or
// CPU exception. Return to the original kernel call stack with the boot address
// space restored. The caller owns scheduling, syscall policy and process exit.
// The unowned entry is retained only for standalone diagnostic fixtures.
Result<Trap> enter(AddressSpace &space, UserFrame &frame);
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &stack);
[[nodiscard]] UserFrame initial_frame(uptr entry, uptr stack);
[[nodiscard]] Syscall syscall(const UserFrame &frame);
void set_return_value(UserFrame &frame, i64 value);

[[nodiscard]] bool user_range(const AddressSpace &space, uptr address, usize size, bool write = false);
Status copy_from_user(const AddressSpace &space, void *destination, uptr source, usize size);
Status copy_to_user(const AddressSpace &space, uptr destination, const void *source, usize size);

} // namespace ok::arch::execution
