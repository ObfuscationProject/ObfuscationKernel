#pragma once

#include "ok/core/types.hpp"

namespace ok::arch::riscv
{

// QEMU virt's CLINT at 0x02000000: one machine software interrupt (MSIP) word
// and one 64-bit mtimecmp per hart, and the shared mtime counter. The kernel
// runs in M-mode, so these are the timer and the IPI line of every hart.
struct Clint
{
    static constexpr uptr msip_base = 0x2000000;
    static constexpr uptr mtimecmp_base = 0x2004000;
    static constexpr uptr mtime = 0x200bff8;

    static uptr hart_id()
    {
        uptr hart;
        asm volatile("csrr %0, mhartid" : "=r"(hart));
        return hart;
    }
    // Normal-memory stores before it (the IPI reasons) reach the target first.
    static void raise_software(uptr hart)
    {
        asm volatile("fence" : : : "memory");
        *reinterpret_cast<volatile u32 *>(msip_base + 4 * hart) = 1;
    }
    // Cleared before the reasons are read, so a later raise is not lost.
    static void clear_software()
    {
        *reinterpret_cast<volatile u32 *>(msip_base + 4 * hart_id()) = 0;
        asm volatile("fence" : : : "memory");
    }
    static void set_compare(u64 value)
    {
        const uptr address = mtimecmp_base + 8 * hart_id();
#if __riscv_xlen == 32
        auto *compare = reinterpret_cast<volatile u32 *>(address);
        compare[0] = 0xffffffffu; // Never below the current time while the halves change.
        compare[1] = static_cast<u32>(value >> 32);
        compare[0] = static_cast<u32>(value);
#else
        *reinterpret_cast<volatile u64 *>(address) = value;
#endif
    }
    static u64 time()
    {
#if __riscv_xlen == 32
        const auto *counter = reinterpret_cast<volatile u32 *>(mtime);
        u32 high, low;
        do
        {
            high = counter[1];
            low = counter[0];
        } while (counter[1] != high);
        return (u64{high} << 32) | low;
#else
        return *reinterpret_cast<volatile u64 *>(mtime);
#endif
    }
};

inline constexpr uptr mie_msie = uptr{1} << 3, mie_mtie = uptr{1} << 7, mie_meie = uptr{1} << 11;

} // namespace ok::arch::riscv
