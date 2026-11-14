#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::ppc>::read_cycle_counter() noexcept
{
#if defined(__powerpc__) || defined(__PPC__)
    u32 value = 0;
    asm volatile("mfspr %0, 268" : "=r"(value));
    return value == 0 ? 1 : value;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::ppc>::memory_fence() noexcept
{
#if defined(__powerpc__) || defined(__PPC__)
    asm volatile("sync\nisync" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::ppc>::enable_interrupts() noexcept
{
}

void NativeInstructions<Architecture::ppc>::disable_interrupts() noexcept
{
}

void NativeInstructions<Architecture::ppc>::halt() noexcept
{
#if defined(__powerpc__) || defined(__PPC__)
    asm volatile("nop" ::: "memory");
#endif
}

// WRTEEI is Book E (the ppce500 e500v2 core), and touches only MSR[EE].
uptr NativeInstructions<Architecture::ppc>::save_and_disable_interrupts() noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && (defined(__powerpc__) || defined(__PPC__))
    u32 msr = 0;
    asm volatile("mfmsr %0\n\twrteei 0" : "=r"(msr) : : "memory");
    return msr & 0x8000;
#else
    return 0;
#endif
}

void NativeInstructions<Architecture::ppc>::restore_interrupts(uptr saved) noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && (defined(__powerpc__) || defined(__PPC__))
    if ((saved & 0x8000) != 0) // MSR[EE]
        asm volatile("wrteei 1" ::: "memory");
#else
    static_cast<void>(saved);
#endif
}

// Every profile reports the boot CPU until per-CPU registers are installed.
u32 NativeInstructions<Architecture::ppc>::cpu_index() noexcept
{
    return 0;
}

} // namespace ok::arch::detail
