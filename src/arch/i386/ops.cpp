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

uptr NativeInstructions<Architecture::i386>::save_and_disable_interrupts() noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__i386__)
    uptr flags = 0;
    asm volatile("pushfl\n\tpopl %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
#else
    return 0;
#endif
}

void NativeInstructions<Architecture::i386>::restore_interrupts(uptr saved) noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__i386__)
    if ((saved & 0x200) != 0) // EFLAGS.IF
        asm volatile("sti" ::: "memory");
#else
    static_cast<void>(saved);
#endif
}

// Every profile reports the boot CPU until per-CPU registers are installed.
u32 NativeInstructions<Architecture::i386>::cpu_index() noexcept
{
    return 0;
}

} // namespace ok::arch::detail
