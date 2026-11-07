#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::arm32>::read_cycle_counter() noexcept
{
#if defined(__arm__)
    u32 value = 0;
    asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(value));
    return value == 0 ? fallback_cycle_counter() : value;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::arm32>::memory_fence() noexcept
{
#if defined(__arm__)
    asm volatile("dsb sy\nisb" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::arm32>::enable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__arm__)
    asm volatile("cpsie i" ::: "memory");
#endif
}

void NativeInstructions<Architecture::arm32>::disable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__arm__)
    asm volatile("cpsid i" ::: "memory");
#endif
}

void NativeInstructions<Architecture::arm32>::halt() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__arm__)
    asm volatile("wfi" ::: "memory");
#elif defined(__arm__)
    asm volatile("yield" ::: "memory");
#endif
}

uptr NativeInstructions<Architecture::arm32>::save_and_disable_interrupts() noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__arm__)
    u32 cpsr = 0;
    asm volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) : : "memory");
    return cpsr;
#else
    return 0;
#endif
}

void NativeInstructions<Architecture::arm32>::restore_interrupts(uptr saved) noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__arm__)
    if ((saved & 0x80) == 0) // CPSR.I clear: IRQs were unmasked.
        asm volatile("cpsie i" ::: "memory");
#else
    static_cast<void>(saved);
#endif
}

// Every profile reports the boot CPU until per-CPU registers are installed.
u32 NativeInstructions<Architecture::arm32>::cpu_index() noexcept
{
    return 0;
}

} // namespace ok::arch::detail
