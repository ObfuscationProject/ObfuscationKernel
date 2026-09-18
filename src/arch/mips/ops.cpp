#include "../ops_private.hpp"

namespace ok::arch::detail
{

u64 NativeInstructions<Architecture::mips>::read_cycle_counter() noexcept
{
#if defined(__mips__)
    u32 value = 0;
    asm volatile("mfc0 %0, $9" : "=r"(value));
    return value == 0 ? 1 : value;
#else
    return fallback_cycle_counter();
#endif
}

void NativeInstructions<Architecture::mips>::memory_fence() noexcept
{
#if defined(__mips__)
    asm volatile("sync" ::: "memory");
#else
    compiler_fence();
#endif
}

void NativeInstructions<Architecture::mips>::enable_interrupts() noexcept
{
}

void NativeInstructions<Architecture::mips>::disable_interrupts() noexcept
{
}

void NativeInstructions<Architecture::mips>::halt() noexcept
{
#if defined(__mips__)
    asm volatile("nop" ::: "memory");
#endif
}

} // namespace ok::arch::detail
