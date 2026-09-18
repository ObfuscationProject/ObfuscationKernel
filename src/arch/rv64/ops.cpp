#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::rv64>::read_cycle_counter() noexcept
{
#if defined(__riscv) && __riscv_xlen == 64
    u64 value = 0;
    asm volatile("rdcycle %0" : "=r"(value));
    return value;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::rv64>::memory_fence() noexcept
{
#if defined(__riscv)
    asm volatile("fence rw, rw" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::rv64>::enable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__riscv)
    asm volatile("csrsi sstatus, 2" ::: "memory");
#endif
}

void NativeInstructions<Architecture::rv64>::disable_interrupts() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__riscv)
    asm volatile("csrci sstatus, 2" ::: "memory");
#endif
}

void NativeInstructions<Architecture::rv64>::halt() noexcept
{
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__riscv)
    asm volatile("wfi" ::: "memory");
#elif defined(__riscv)
    asm volatile("nop" ::: "memory");
#endif
}

} // namespace ok::arch::detail
