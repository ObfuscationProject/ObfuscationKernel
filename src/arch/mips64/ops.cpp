#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::mips64>::read_cycle_counter() noexcept
{
#if defined(__mips__)
    u32 value = 0;
    asm volatile("mfc0 %0, $9" : "=r"(value));
    return value == 0 ? 1 : value;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::mips64>::memory_fence() noexcept
{
#if defined(__mips__)
    asm volatile("sync" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::mips64>::enable_interrupts() noexcept
{
}

void NativeInstructions<Architecture::mips64>::disable_interrupts() noexcept
{
}

void NativeInstructions<Architecture::mips64>::halt() noexcept
{
#if defined(__mips__)
    asm volatile("nop" ::: "memory");
#endif
}

uptr NativeInstructions<Architecture::mips64>::save_and_disable_interrupts() noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__mips__)
    u32 status = 0;
    asm volatile("di %0\n\tehb" : "=r"(status) : : "memory");
    return status & 1;
#else
    return 0;
#endif
}

void NativeInstructions<Architecture::mips64>::restore_interrupts(uptr saved) noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__mips__)
    if ((saved & 1) != 0) // Status.IE
        asm volatile("ei\n\tehb" ::: "memory");
#else
    static_cast<void>(saved);
#endif
}

// Every profile reports the boot CPU until per-CPU registers are installed.
u32 NativeInstructions<Architecture::mips64>::cpu_index() noexcept
{
    return 0;
}

} // namespace ok::arch::detail
