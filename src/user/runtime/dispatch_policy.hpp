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

// Only builds with kernel sections compiled in (x86_64, aarch64 and rv64 with
// smp_sections=y) may dispatch a row unlocked; every other build forces the
// lock on every row.
#if defined(OK_SMP_SECTIONS)
inline constexpr bool unlocked_dispatch_built = true;
#else
inline constexpr bool unlocked_dispatch_built = false;
#endif

template <lu::Profile P> [[nodiscard]] constexpr bool bkl_required(lu::Operation operation) noexcept
{
    const auto index = static_cast<usize>(operation);
    return !unlocked_dispatch_built || index >= linux_operation_count ||
           linux_lock_modes<P>[index] == LockMode::bkl;
}

// Unknown numbers stay under the lock.
[[nodiscard]] constexpr bool native_bkl_required(u64 number) noexcept
{
    return !unlocked_dispatch_built || number >= native_syscall_limit ||
           native_lock_modes[static_cast<usize>(number)] == LockMode::bkl;
}

} // namespace ok::user::native::dispatch_policy
