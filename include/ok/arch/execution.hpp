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
// Every page of the user window can carry one mapping record.
inline constexpr usize user_window_pages = (user_end - user_begin) / page_size;

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

// Contiguous records in strictly ascending virtual-address order. A backend
// may retain the view while the address space it created from it exists: the
// owner neither moves nor edits published records, except one record at a time
// through remap(). Zero-flag records retain backing for PROT_NONE but must not
// acquire a user translation.
struct MappingView
{
    const PageMapping *records{};
    usize count{};

    constexpr MappingView() = default;
    constexpr MappingView(std::span<const PageMapping> contiguous)
        : records(contiguous.data()), count(contiguous.size()) {}
    constexpr MappingView(const PageMapping *data, usize size) : records(data), count(size) {}
    template <usize N> constexpr MappingView(const std::array<PageMapping, N> &data)
        : MappingView(data.data(), N) {}
    template <usize N> constexpr MappingView(const PageMapping (&data)[N])
        : MappingView(data, N) {}
    [[nodiscard]] constexpr usize size() const { return count; }
    [[nodiscard]] constexpr bool empty() const { return count == 0; }
    // Validate the public directory before a backend indexes it: bounded by
    // the user window, present when nonempty and strictly ascending, which
    // also excludes duplicate pages.
    [[nodiscard]] constexpr bool valid() const
    {
        if (count > user_window_pages || (count && !records)) return false;
        for (usize index = 1; index < count; ++index)
            if (records[index].virtual_address <= records[index - 1].virtual_address) return false;
        return true;
    }
    [[nodiscard]] constexpr const PageMapping &operator[](usize index) const { return records[index]; }
    [[nodiscard]] constexpr const PageMapping *begin() const { return records; }
    [[nodiscard]] constexpr const PageMapping *end() const { return records + count; }
    // The record of the page holding `address`, or null.
    [[nodiscard]] constexpr const PageMapping *find(uptr address) const
    {
        const uptr page = address & ~uptr{page_size - 1};
        usize low = 0, high = count;
        while (low < high)
        {
            const usize middle = low + (high - low) / 2;
            if (records[middle].virtual_address < page) low = middle + 1;
            else high = middle;
        }
        return low < count && records[low].virtual_address == page ? &records[low] : nullptr;
    }
};

struct AddressSpace
{
    uptr root{0};
    u32 slot{0};
    u32 generation{0};
};

// Serialized boot-CPU snapshot of backend slots, independent of process records.
// Includes allocated slots whose handles were never published to a process.
// `table_pages` counts the table-storage pages the backend holds, including its
// slot table; static kernel tables are excluded. These backends retire
// synchronously; runtime page/pin/quarantine owners are reported separately.
struct OwnershipReport
{
    Status failure{};
    usize address_spaces{};
    usize table_pages{};
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
    // A device interrupt preempted user code; its top half already ran. The
    // thread resumes like a timer preemption but is not charged a CPU tick.
    interrupt,
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
// Translation-table storage, supplied by the native runtime that owns user
// memory: `pages` zero-filled frames whose first address is aligned to
// `alignment` and is both the supervisor pointer and the address the
// translation hardware walks. Backends hold tables and slot records here only
// while an address space needs them and release each run as allocated.
[[nodiscard]] Result<uptr> allocate_table_pages(usize pages, usize alignment = page_size);
Status release_table_pages(uptr address, usize pages);
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
