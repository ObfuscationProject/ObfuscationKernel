#pragma once

#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"

#include <cstddef>

// What a secondary CPU reads before its MMU is on (boot.S's
// ok_aarch64_secondary_entry), and the execution backend's part in filling it.
namespace ok::arch::aarch64
{

struct alignas(64) SecondaryStart
{
    uptr stack_top{};
    uptr cpu_block{};
    u64 mair{};
    u64 tcr{};
    u64 vbar{};
    u64 cpacr{};
    u64 ttbr0{};
    u64 sctlr{};
    uptr entry{};
    u64 cpu{};
};
static_assert(offsetof(SecondaryStart, stack_top) == 0 && offsetof(SecondaryStart, cpu_block) == 8);
static_assert(offsetof(SecondaryStart, mair) == 16 && offsetof(SecondaryStart, tcr) == 24);
static_assert(offsetof(SecondaryStart, vbar) == 32 && offsetof(SecondaryStart, cpacr) == 40);
static_assert(offsetof(SecondaryStart, ttbr0) == 48 && offsetof(SecondaryStart, sctlr) == 56);
static_assert(offsetof(SecondaryStart, entry) == 64 && offsetof(SecondaryStart, cpu) == 72);

// Resets logical CPU `cpu`'s per-CPU block and fills the kernel translation,
// vectors and system control values of `start`. Needs an initialized backend.
Status prepare_secondary(ok::smp::CpuId cpu, SecondaryStart &start);

} // namespace ok::arch::aarch64
