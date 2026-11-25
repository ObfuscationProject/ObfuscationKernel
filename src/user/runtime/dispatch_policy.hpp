#pragma once

// Which syscalls the step dispatches under the big kernel lock. One table per
// Linux profile, indexed by operation, and one for the native personality,
// indexed by syscall number; a family's rows change to `unlocked` only in the
// package that takes its handlers off the lock. Every row requires the lock
// until then, and nothing consults the tables yet.
#include "ok/abi/linux_profile_data.hpp"
#include "ok/core/types.hpp"
#include "ok/syscall/syscall.hpp"

#include <array>

namespace ok::user::native::dispatch_policy
{
namespace lu = abi::linux_user;

enum class LockMode : u8
{
    bkl,      // Dispatched holding the big kernel lock.
    unlocked, // Dispatched without it; the handler takes its own locks.
};

inline constexpr usize linux_operation_count = static_cast<usize>(lu::Operation::count);
inline constexpr usize profile_count = static_cast<usize>(lu::Profile::count);
// Native syscall numbers are sparse; pty_pair is the highest.
inline constexpr usize native_syscall_limit = static_cast<usize>(syscall::Number::pty_pair) + 1;

using LinuxLockTable = std::array<LockMode, linux_operation_count>;
using NativeLockTable = std::array<LockMode, native_syscall_limit>;

template <typename Table> [[nodiscard]] consteval Table all_bkl()
{
    Table table{};
    for (auto &mode : table)
        mode = LockMode::bkl;
    return table;
}

template <lu::Profile P> inline constexpr LinuxLockTable linux_lock_modes = all_bkl<LinuxLockTable>();
inline constexpr NativeLockTable native_lock_modes = all_bkl<NativeLockTable>();

// The smp_sections=n arm (later smp_unlocked=n) and the private futex peer
// fixture force the lock on every row, on every profile (xmake/arch.lua
// add_ok_smp_sections defines OK_BKL_DISPATCH_FORCED). Every other build,
// UP profiles included, follows the tables.
#if defined(OK_BKL_DISPATCH_FORCED)
inline constexpr bool bkl_dispatch_forced = true;
#else
inline constexpr bool bkl_dispatch_forced = false;
#endif

// One row of a table; indices past its end stay under the lock.
template <typename Table>
[[nodiscard]] constexpr bool row_requires_bkl(const Table &table, usize index, bool forced) noexcept
{
    return forced || index >= table.size() || table[index] == LockMode::bkl;
}

template <lu::Profile P> [[nodiscard]] constexpr bool bkl_required(lu::Operation operation) noexcept
{
    return row_requires_bkl(linux_lock_modes<P>, static_cast<usize>(operation), bkl_dispatch_forced);
}

// Unknown numbers stay under the lock.
[[nodiscard]] constexpr bool native_bkl_required(u64 number) noexcept
{
    return number >= native_syscall_limit ||
           row_requires_bkl(native_lock_modes, static_cast<usize>(number), bkl_dispatch_forced);
}

} // namespace ok::user::native::dispatch_policy
