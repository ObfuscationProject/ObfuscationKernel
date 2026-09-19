#pragma once

#include "ok/core/types.hpp"

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
};

struct AddressSpace
{
    uptr root{0};
    u32 slot{0};
    u32 generation{0};
};

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
};

[[nodiscard]] bool supported() noexcept;
Status initialize();
Result<AddressSpace> create_space(std::span<const PageMapping> mappings);
Status destroy_space(AddressSpace &space);

// Execute at user privilege until a real syscall, hardware timer interrupt or
// CPU exception. Return to the original kernel call stack with the boot address
// space restored. The caller owns scheduling, syscall policy and process exit.
Result<Trap> enter(AddressSpace &space, UserFrame &frame);
[[nodiscard]] UserFrame initial_frame(uptr entry, uptr stack);
[[nodiscard]] Syscall syscall(const UserFrame &frame);
void set_return_value(UserFrame &frame, i64 value);

[[nodiscard]] bool user_range(const AddressSpace &space, uptr address, usize size, bool write = false);
Status copy_from_user(const AddressSpace &space, void *destination, uptr source, usize size);
Status copy_to_user(const AddressSpace &space, uptr destination, const void *source, usize size);

} // namespace ok::arch::execution
