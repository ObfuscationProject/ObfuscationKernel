#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::i386>::read_cycle_counter() noexcept
{
#if defined(__i386__) || defined(__x86_64__)
    u32 low = 0;
    u32 high = 0;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<u64>(high) << 32) | low;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::i386>::memory_fence() noexcept
{
#if defined(__x86_64__)
    asm volatile("mfence" ::: "memory");
#elif defined(__i386__)
    asm volatile("lock; addl $0, 0(%%esp)" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::i386>::enable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__i386__)
    asm volatile("sti" ::: "memory");
#endif
}

void NativeInstructions<Architecture::i386>::disable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__i386__)
    asm volatile("cli" ::: "memory");
#endif
}

void NativeInstructions<Architecture::i386>::halt() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__i386__)
    asm volatile("hlt" ::: "memory");
#elif defined(__i386__) || defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#endif
}

} // namespace ok::arch::detail
