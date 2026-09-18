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

} // namespace ok::arch::detail
