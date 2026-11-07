#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::loongarch64>::read_cycle_counter() noexcept
{
#if defined(__loongarch__) && defined(__loongarch64)
    u64 value = 0;
    asm volatile("rdtime.d %0, $r0" : "=r"(value));
    return value == 0 ? fallback_cycle_counter() : value;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::loongarch64>::memory_fence() noexcept
{
#if defined(__loongarch__)
    asm volatile("dbar 0" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::loongarch64>::enable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__loongarch__)
    asm volatile("csrxchg $r0, $r0, 0x0" ::: "memory");
#endif
}

void NativeInstructions<Architecture::loongarch64>::disable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__loongarch__)
    asm volatile("csrxchg $r0, $r0, 0x0" ::: "memory");
#endif
}

void NativeInstructions<Architecture::loongarch64>::halt() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__loongarch__)
    asm volatile("idle 0" ::: "memory");
#elif defined(__loongarch__)
    asm volatile("nop" ::: "memory");
#endif
}

// CSRXCHG returns the old CRMD and writes only the bits its mask (never $r0/$r1) selects.
uptr NativeInstructions<Architecture::loongarch64>::save_and_disable_interrupts() noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__loongarch__)
    uptr crmd = 0;
    asm volatile("csrxchg %0, %1, 0x0" : "+r"(crmd) : "q"(uptr{4}) : "memory");
    return crmd & 4;
#else
    return 0;
#endif
}

void NativeInstructions<Architecture::loongarch64>::restore_interrupts(uptr saved) noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__loongarch__)
    uptr crmd = saved & 4; // CRMD.IE
    if (crmd != 0)
        asm volatile("csrxchg %0, %1, 0x0" : "+r"(crmd) : "q"(uptr{4}) : "memory");
#else
    static_cast<void>(saved);
#endif
}

// Every profile reports the boot CPU until per-CPU registers are installed.
u32 NativeInstructions<Architecture::loongarch64>::cpu_index() noexcept
{
    return 0;
}

} // namespace ok::arch::detail
