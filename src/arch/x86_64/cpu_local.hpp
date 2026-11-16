#pragma once

#include "../tlb_tags.hpp"
#include "ok/arch/execution.hpp"
#include "ok/smp/smp.hpp"

namespace ok::arch::execution::x86
{

struct [[gnu::packed]] TaskState
{
    u32 reserved0{};
    u64 rsp[3]{};
    u64 reserved1{};
    u64 ist[7]{};
    u64 reserved2{};
    u16 reserved3{};
    u16 io_bitmap_offset{sizeof(TaskState)};
};
static_assert(sizeof(TaskState) == 104);

// One CPU's entry, trap and syscall state. In kernel mode GS_BASE holds the
// running CPU's block on every CPU; execution.S reaches the leading fields by
// offset (checked in execution.cpp), ops.cpp reads `logical`.
struct alignas(64) CpuLocal
{
    CpuLocal *self{};
    u64 logical{};
    uptr resume_rsp{};
    uptr user_rsp{};
    uptr stack_top{}; // SYSCALL stack, the entry's TSS.rsp0
    UserFrame *active_frame{};
    u64 kernel_fs{};
    u64 apic_id{};
    alignas(16) u8 kernel_fp[512]{};
    alignas(16) u64 gdt[7]{};
    TaskState tss{};
    bool executing{};
    Trap last_trap{};
    // This CPU's TSC minus the boot CPU's, as bring-up measured it when
    // IA32_TSC_ADJUST could not remove it; clock reads subtract it.
    u64 tsc_offset{};
    // PCIDs this CPU gives user spaces; untagged without CR4.PCIDE.
    ok::arch::detail::CpuTlbTags tlb_tags{};
};
inline constexpr usize cpu_logical_offset = 8;

extern "C" CpuLocal ok_x86_cpu_locals[ok::smp::max_cpus];

[[nodiscard]] inline CpuLocal &this_cpu() noexcept
{
    CpuLocal *self;
    asm volatile("mov %%gs:0, %0" : "=r"(self));
    return *self;
}

// The TSC, read after earlier instructions and before later ones where LFENCE
// is dispatch-serializing (Intel; AMD only with DE_CFG[1], which this kernel
// does not set). Unlike the CPUID the i386 clock serializes with, LFENCE
// causes no VM exit. The shared clock's floor, not this fence, orders
// readings across CPUs; the TSC handshake adds MFENCE (smp.cpp).
[[nodiscard]] inline u64 read_tsc_ordered() noexcept
{
    u32 low, high;
    asm volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) : : "memory");
    return (u64{high} << 32) | low;
}

// The platform clock's counter: this CPU's TSC corrected to the boot CPU's.
// Kernel code is not migrated, so the offset belongs to the CPU that read
// the TSC.
[[nodiscard]] inline u64 clock_counter() noexcept
{
    const u64 counter = read_tsc_ordered();
#if defined(OK_KERNEL_FREESTANDING)
    u64 offset;
    asm volatile("mov %%gs:%c1, %0" : "=r"(offset) : "i"(__builtin_offsetof(CpuLocal, tsc_offset)) : "memory");
    return counter - offset;
#else
    return counter;
#endif
}

// The boot CPU fills `cpu`'s block and descriptor tables before starting it.
void prepare_cpu(ok::smp::CpuId cpu, u32 apic_id);
// On `cpu` itself, which runs on the boot translation: load its GDT and TSS,
// the shared IDT, GS_BASE and the MSR/CR state initialize() set on the boot CPU.
void load_cpu(ok::smp::CpuId cpu);

} // namespace ok::arch::execution::x86
