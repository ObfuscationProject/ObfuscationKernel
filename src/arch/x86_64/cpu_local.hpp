#pragma once

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
};
inline constexpr usize cpu_logical_offset = 8;

extern "C" CpuLocal ok_x86_cpu_locals[ok::smp::max_cpus];

[[nodiscard]] inline CpuLocal &this_cpu() noexcept
{
    CpuLocal *self;
    asm volatile("mov %%gs:0, %0" : "=r"(self));
    return *self;
}

// The boot CPU fills `cpu`'s block and descriptor tables before starting it.
void prepare_cpu(ok::smp::CpuId cpu, u32 apic_id);
// On `cpu` itself, which runs on the boot translation: load its GDT and TSS,
// the shared IDT, GS_BASE and the MSR/CR state initialize() set on the boot CPU.
void load_cpu(ok::smp::CpuId cpu);

} // namespace ok::arch::execution::x86
