#pragma once

#include "ok/arch/types.hpp"

#include <atomic>

namespace ok::arch::detail
{

// Unprivileged and inline, so spin loops in shared headers link without the
// arch library: x86 PAUSE, Arm YIELD, RISC-V Zihintpause PAUSE (a FENCE hint
// on cores without it), a compiler fence elsewhere.
inline void spin_pause_hint() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#elif defined(__aarch64__) || (defined(__arm__) && __ARM_ARCH >= 7)
    asm volatile("yield" ::: "memory");
#elif defined(__riscv)
    asm volatile(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Native instruction entry points are selected by template argument, never by a vtable.
template <Architecture A> struct NativeInstructions;

template <> struct NativeInstructions<Architecture::i386>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    static u32 cpu_index() noexcept;
};

template <> struct NativeInstructions<Architecture::x86_64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    static u32 cpu_index() noexcept;
};

template <> struct NativeInstructions<Architecture::aarch64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    // Inline for the same reason: the logical id heads the per-CPU block that
    // TPIDR_EL1 names from boot.S on (src/arch/aarch64/execution.cpp).
    static u32 cpu_index() noexcept
    {
#if defined(OK_KERNEL_FREESTANDING) && defined(__aarch64__)
        uptr block;
        asm("mrs %0, tpidr_el1" : "=r"(block));
        // Images without the native backend have no block.
        return block != 0 ? *reinterpret_cast<const u32 *>(block) : 0;
#else
        return 0;
#endif
    }
};

template <> struct NativeInstructions<Architecture::arm32>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    static u32 cpu_index() noexcept;
};

template <> struct NativeInstructions<Architecture::rv64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    static u32 cpu_index() noexcept;
};

template <> struct NativeInstructions<Architecture::rv32>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    static u32 cpu_index() noexcept;
};

template <> struct NativeInstructions<Architecture::loongarch64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    static u32 cpu_index() noexcept;
};

template <> struct NativeInstructions<Architecture::mips>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    static u32 cpu_index() noexcept;
};

template <> struct NativeInstructions<Architecture::mips64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    static u32 cpu_index() noexcept;
};

template <> struct NativeInstructions<Architecture::ppc>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
    static uptr save_and_disable_interrupts() noexcept;
    static void restore_interrupts(uptr saved) noexcept;
    static void spin_pause() noexcept
    {
        spin_pause_hint();
    }
    static u32 cpu_index() noexcept;
};

} // namespace ok::arch::detail
