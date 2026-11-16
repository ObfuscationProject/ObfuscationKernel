#pragma once

// Futex user-word operations through the kernel pointer to the word's backing
// frame (arch::execution::user_word), so each read-modify-write is one atomic
// operation with respect to user code running on other CPUs.
//
// The resolver keeps the word mapped only for the duration of one call; the
// runtime holds the kernel lock across it, which every remap, unmap and space
// teardown also takes. A caller without that lock must hold the process's
// address-space lock instead: a frame pin keeps the frame allocated but does
// not stop a copy-on-write remap from replacing it.
#include "ok/abi/linux_futex_wake_operation.hpp"
#include "ok/core/types.hpp"

#include <bit>
#include <concepts>

namespace ok::user::native
{
// A 32-bit compare-exchange is inline on every profile except i386, which has
// no CMPXCHG and runs UP: there kernel code with interrupts masked is
// indivisible against user code, so plain aligned loads and stores suffice.
// The SDK's smp_atomic = 1 relies on this; a profile that starts secondary
// CPUs (OK_ARCH_SMP_START) must have the hardware operation.
#if defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
inline constexpr bool futex_word_hardware_atomic = true;
#elif defined(OK_ARCH_TARGET_I386) && !defined(OK_ARCH_SMP_START)
inline constexpr bool futex_word_hardware_atomic = false;
#else
#error "futex word operations need a 32-bit compare-exchange on a profile that may run SMP"
#endif

// Resolves an aligned user word to its kernel pointer; `write` resolves
// copy-on-write first. Errors are the copy path's: fault when not accessible.
template <typename Resolve>
concept FutexWordResolver = requires(Resolve &resolve, uptr address, bool write) {
    { resolve(address, write) } -> std::same_as<Result<u32 *>>;
};

// Single-word primitives with sequentially consistent ordering.
template <bool HardwareAtomic> struct FutexWordAccess
{
    static u32 load(const u32 *word) { return __atomic_load_n(word, __ATOMIC_SEQ_CST); }
    static void store(u32 *word, u32 value) { __atomic_store_n(word, value, __ATOMIC_SEQ_CST); }
    // Returns the value found; `desired` was stored iff it equals `expected`.
    static u32 compare_exchange(u32 *word, u32 expected, u32 desired)
    {
        if constexpr (HardwareAtomic)
        {
            __atomic_compare_exchange_n(word, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            return expected;
        }
        else
        {
            const u32 actual = load(word);
            if (actual == expected) store(word, desired);
            return actual;
        }
    }
    // FUTEX_OP arithmetic 0..4 (SET, ADD, OR, ANDN, XOR); returns the old value.
    static u32 fetch(u32 *word, const abi::linux_user::FutexWakeOperation &operation)
    {
        const u32 value = operation.argument();
        if constexpr (HardwareAtomic)
        {
            switch (operation.operation)
            {
            case 0: return __atomic_exchange_n(word, value, __ATOMIC_SEQ_CST);
            case 1: return __atomic_fetch_add(word, value, __ATOMIC_SEQ_CST);
            case 2: return __atomic_fetch_or(word, value, __ATOMIC_SEQ_CST);
            case 3: return __atomic_fetch_and(word, ~value, __ATOMIC_SEQ_CST);
            default: return __atomic_fetch_xor(word, value, __ATOMIC_SEQ_CST);
            }
        }
        else
        {
            const u32 previous = load(word);
            store(word, operation.modify(previous).value());
            return previous;
        }
    }
};

// Converts between the profile's word byte order and the CPU's. They differ
// only in hosted models of a big-endian profile on a little-endian host.
[[nodiscard]] constexpr bool futex_word_native_order(bool big_endian)
{
    return big_endian == (std::endian::native == std::endian::big);
}
[[nodiscard]] constexpr u32 futex_word_order(u32 value, bool big_endian)
{
    return futex_word_native_order(big_endian) ? value : std::byteswap(value);
}

namespace futex_word_detail
{
template <FutexWordResolver Resolve> Result<u32 *> resolve(Resolve &resolve, uptr address, bool write)
{
    if (address % sizeof(u32)) return Status::invalid_argument("futex word is misaligned");
    return resolve(address, write);
}
} // namespace futex_word_detail

// One atomic load of the word, for the FUTEX_WAIT and CMP_REQUEUE compares.
template <bool HardwareAtomic = futex_word_hardware_atomic, FutexWordResolver Resolve>
Result<u32> futex_word_load(Resolve &&resolve, uptr address, bool big_endian)
{
    auto word = futex_word_detail::resolve(resolve, address, false);
    if (!word) return word.status();
    return futex_word_order(FutexWordAccess<HardwareAtomic>::load(word.value()), big_endian);
}

// One atomic store, for clear_child_tid.
template <bool HardwareAtomic = futex_word_hardware_atomic, FutexWordResolver Resolve>
Status futex_word_store(Resolve &&resolve, uptr address, u32 value, bool big_endian)
{
    auto word = futex_word_detail::resolve(resolve, address, true);
    if (!word) return word.status();
    FutexWordAccess<HardwareAtomic>::store(word.value(), futex_word_order(value, big_endian));
    return Status::success();
}

// The robust-list owner update: returns the value found, which equals
// `expected` iff `desired` was stored.
template <bool HardwareAtomic = futex_word_hardware_atomic, FutexWordResolver Resolve>
Result<u32> futex_word_compare_exchange(Resolve &&resolve, uptr address, u32 expected, u32 desired,
                                        bool big_endian)
{
    auto word = futex_word_detail::resolve(resolve, address, true);
    if (!word) return word.status();
    const auto actual = FutexWordAccess<HardwareAtomic>::compare_exchange(
        word.value(), futex_word_order(expected, big_endian), futex_word_order(desired, big_endian));
    return futex_word_order(actual, big_endian);
}

// FUTEX_WAKE_OP's update of the second word as one atomic read-modify-write;
// returns the old value for the comparison. An undefined arithmetic operation
// is rejected before the word is resolved or touched.
template <bool HardwareAtomic = futex_word_hardware_atomic, FutexWordResolver Resolve>
Result<u32> futex_word_wake_operation(Resolve &&resolve, uptr address,
                                      const abi::linux_user::FutexWakeOperation &operation, bool big_endian)
{
    using Access = FutexWordAccess<HardwareAtomic>;
    if (operation.operation > 4) return Status::unsupported("Linux futex wake operation is not defined");
    auto word = futex_word_detail::resolve(resolve, address, true);
    if (!word) return word.status();
    if (futex_word_native_order(big_endian)) return Access::fetch(word.value(), operation);
    // A byte-swapped word cannot use fetch-add; retry a compare-exchange.
    u32 raw = Access::load(word.value());
    for (;;)
    {
        const u32 previous = futex_word_order(raw, big_endian);
        const u32 next = futex_word_order(operation.modify(previous).value(), big_endian);
        const u32 actual = Access::compare_exchange(word.value(), raw, next);
        if (actual == raw) return previous;
        raw = actual;
    }
}
} // namespace ok::user::native
