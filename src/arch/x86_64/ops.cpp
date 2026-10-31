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

uptr NativeInstructions<Architecture::x86_64>::save_and_disable_interrupts() noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__x86_64__)
    uptr flags = 0;
    asm volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
#else
    return 0;
#endif
}

void NativeInstructions<Architecture::x86_64>::restore_interrupts(uptr saved) noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__x86_64__)
    if ((saved & 0x200) != 0) // RFLAGS.IF
        asm volatile("sti" ::: "memory");
#else
    static_cast<void>(saved);
#endif
}

// Every profile reports the boot CPU until per-CPU registers are installed.
u32 NativeInstructions<Architecture::x86_64>::cpu_index() noexcept
{
    return 0;
}

} // namespace ok::arch::detail
