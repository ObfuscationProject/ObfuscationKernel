#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::rv32>::read_cycle_counter() noexcept
{
#if defined(__riscv) && __riscv_xlen == 32
    u32 high_before = 0;
    u32 low = 0;
    u32 high_after = 0;
    do
    {
        asm volatile("rdcycleh %0" : "=r"(high_before));
        asm volatile("rdcycle %0" : "=r"(low));
        asm volatile("rdcycleh %0" : "=r"(high_after));
    } while (high_before != high_after);
    return (static_cast<u64>(high_after) << 32) | low;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::rv32>::memory_fence() noexcept
{
#if defined(__riscv)
    asm volatile("fence rw, rw" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::rv32>::enable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__riscv)
    asm volatile("csrsi sstatus, 2" ::: "memory");
#endif
}

void NativeInstructions<Architecture::rv32>::disable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__riscv)
    asm volatile("csrci sstatus, 2" ::: "memory");
#endif
}

void NativeInstructions<Architecture::rv32>::halt() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__riscv)
    asm volatile("wfi" ::: "memory");
#elif defined(__riscv)
    asm volatile("nop" ::: "memory");
#endif
}

} // namespace ok::arch::detail
