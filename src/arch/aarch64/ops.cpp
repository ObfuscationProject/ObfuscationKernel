#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::aarch64>::read_cycle_counter() noexcept
{
#if defined(__aarch64__)
    u64 value = 0;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::aarch64>::memory_fence() noexcept
{
#if defined(__aarch64__)
    asm volatile("dsb sy\nisb" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::aarch64>::enable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__aarch64__)
    asm volatile("msr daifclr, #2" ::: "memory");
#endif
}

void NativeInstructions<Architecture::aarch64>::disable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__aarch64__)
    asm volatile("msr daifset, #2" ::: "memory");
#endif
}

void NativeInstructions<Architecture::aarch64>::halt() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__aarch64__)
    asm volatile("wfi" ::: "memory");
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

} // namespace ok::arch::detail
