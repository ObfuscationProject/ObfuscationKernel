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

uptr NativeInstructions<Architecture::aarch64>::save_and_disable_interrupts() noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__aarch64__)
    uptr daif = 0;
    asm volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(daif) : : "memory");
    return daif;
#else
    return 0;
#endif
}

void NativeInstructions<Architecture::aarch64>::restore_interrupts(uptr saved) noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__aarch64__)
    if ((saved & 0x80) == 0) // DAIF.I clear: IRQs were unmasked.
        asm volatile("msr daifclr, #2" ::: "memory");
#else
    static_cast<void>(saved);
#endif
}

// Every profile reports the boot CPU until per-CPU registers are installed.
u32 NativeInstructions<Architecture::aarch64>::cpu_index() noexcept
{
    return 0;
}

} // namespace ok::arch::detail
