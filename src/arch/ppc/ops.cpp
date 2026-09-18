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

} // namespace ok::arch::detail
