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

// The kernel runs in machine mode (QEMU -bios none): the mask is mstatus.MIE.
uptr NativeInstructions<Architecture::rv64>::save_and_disable_interrupts() noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__riscv)
    uptr status = 0;
    asm volatile("csrrci %0, mstatus, 8" : "=r"(status) : : "memory");
    return status & 8;
#else
    return 0;
#endif
}

void NativeInstructions<Architecture::rv64>::restore_interrupts(uptr saved) noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__riscv)
    asm volatile("csrs mstatus, %0" : : "r"(saved & 8) : "memory");
#else
    static_cast<void>(saved);
#endif
}

// The logical id heads the per-CPU block tp points at (src/arch/rv64/
// execution.cpp); boot.S leaves tp 0 in images without the native backend.
u32 NativeInstructions<Architecture::rv64>::cpu_index() noexcept
{
#if defined(OK_KERNEL_FREESTANDING) && defined(__riscv)
    const uptr *block;
    asm("mv %0, tp" : "=r"(block));
    return block != nullptr ? static_cast<u32>(*block) : 0;
#else
    return 0;
#endif
}

} // namespace ok::arch::detail
