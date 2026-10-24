#pragma once

#include "ok/core/types.hpp"
#include <concepts>

namespace ok::sched
{

// Per-thread canonical Linux alternate-stack state, never a mapped-memory
// ownership claim. All ten native profiles have downward-growing stacks.
struct SignalAltstack
{
    static constexpr u32 onstack = 1;
    static constexpr u32 disable = 2;
    static constexpr u32 autodisarm = 0x80000000U;
    u64 base{};
    u64 size{};
    u32 flags{disable};
    constexpr bool operator==(const SignalAltstack &) const = default;

    [[nodiscard]] constexpr bool physically_contains(u64 sp) const noexcept
    {
        // Exclude the bottom, include the top. Subtraction avoids base+size
        // overflow; installing a wrapping/unmapped range is not rejected here.
        return sp > base && sp - base <= size;
    }
    [[nodiscard]] constexpr bool on_stack(u64 sp) const noexcept
    {
        return !(flags & autodisarm) && physically_contains(sp);
    }
    [[nodiscard]] constexpr u32 status(u64 sp) const noexcept
    {
        return !size ? disable : on_stack(sp) ? onstack : 0;
    }
    [[nodiscard]] constexpr SignalAltstack query(u64 sp) const noexcept
    {
        return {base, size, status(sp) | (flags & autodisarm)};
    }

    // Linux do_sigaltstack: a size policy models architecture/runtime feature
    // requirements (notably dynamic x86 FP frames). The caller serializes it
    // with permission/feature changes. No default policy silently bypasses it.
    template <typename SizeValid>
        requires requires(SizeValid &valid, u64 bytes) {
            { valid(bytes) } noexcept -> std::same_as<bool>;
        }
    [[nodiscard]] constexpr i64 apply(SignalAltstack requested, u64 sp, u64 minimum,
                                      SizeValid &&valid) noexcept
    {
        if (on_stack(sp)) return -1;
        const u32 mode = requested.flags & ~autodisarm;
        if (mode != disable && mode != onstack && mode != 0) return -22;
        // This precedes the size policy, even if feature requirements changed.
        if (*this == requested) return 0;
        if (mode == disable)
        {
            requested.base = 0;
            requested.size = 0;
        }
        else
        {
            const bool valid_size = valid(requested.size);
            if (requested.size < minimum || !valid_size) return -12;
        }
        *this = requested;
        return 0;
    }

    constexpr void reset() noexcept { *this = {}; }
    // Successful exec clears the region only; Linux retains sas_ss_flags.
    // A failed exec must not call this operation.
    constexpr void clear_for_exec() noexcept { base = size = 0; }
    [[nodiscard]] constexpr SignalAltstack copy_for_clone(bool shares_vm, bool vfork) const noexcept
    {
        return shares_vm && !vfork ? SignalAltstack{} : *this;
    }
    // Snapshot the raw state in ucontext before this call. Invoke only after
    // the entire signal frame and handler context have successfully committed,
    // even when this signal did not request SA_ONSTACK.
    constexpr void disarm_after_frame_commit() noexcept
    {
        if (flags & autodisarm) reset();
    }
};

} // namespace ok::sched
