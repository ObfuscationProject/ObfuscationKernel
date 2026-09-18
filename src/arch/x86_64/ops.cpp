#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::x86_64>::read_cycle_counter() noexcept
{
#if defined(__x86_64__)
    u32 low = 0;
    u32 high = 0;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<u64>(high) << 32) | low;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::x86_64>::memory_fence() noexcept
{
#if defined(__x86_64__)
    asm volatile("mfence" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::x86_64>::enable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__x86_64__)
    asm volatile("sti" ::: "memory");
#endif
}

void NativeInstructions<Architecture::x86_64>::disable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__x86_64__)
    asm volatile("cli" ::: "memory");
#endif
}

void NativeInstructions<Architecture::x86_64>::halt() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__x86_64__)
    asm volatile("hlt" ::: "memory");
#elif defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#endif
}

} // namespace ok::arch::detail
