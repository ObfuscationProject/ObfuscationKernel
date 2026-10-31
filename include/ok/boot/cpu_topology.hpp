#pragma once

#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <string_view>

namespace ok::boot
{

// How a secondary CPU can be released. Discovery only names the mechanism the
// firmware description supports; no CPU is started by parsing.
enum class CpuStartMethod : u8
{
    none,
    x86_sipi,
    psci_hvc,
    psci_smc,
    riscv_msip,
};

struct DiscoveredCpu
{
    u64 hardware_id{}; // Local APIC/x2APIC id, MPIDR affinity or RISC-V hart id.
    bool enabled{};
    bool boot{};
};

struct CpuTopologyMap
{
    std::array<DiscoveredCpu, smp::max_cpus> cpus{};
    usize count{};
    usize boot_index{};
    bool boot_known{};
    CpuStartMethod method{CpuStartMethod::none};
    u32 psci_cpu_on{};
    u64 timebase_hz{};
    u64 local_apic_address{};
    bool truncated{}; // The firmware described more usable CPUs than max_cpus.
    // Non-empty exactly when method is none: the platform must run UP, e.g.
    // "single-cpu", "psci-absent", "boot-cpu-not-described".
    std::string_view up_reason{};

    [[nodiscard]] constexpr usize enabled_count() const
    {
        usize result = 0;
        for (usize i = 0; i < count; ++i)
            result += cpus[i].enabled ? 1 : 0;
        return result;
    }
};

// Identify the running CPU by its own hardware id. Failure (the id is absent or
// describes a disabled CPU) leaves no boot CPU and downgrades the map to UP.
constexpr bool mark_boot_cpu(CpuTopologyMap &map, u64 hardware_id)
{
    map.boot_known = false;
    map.boot_index = 0;
    for (usize i = 0; i < map.count; ++i)
        map.cpus[i].boot = false;
    for (usize i = 0; i < map.count; ++i)
    {
        if (map.cpus[i].hardware_id != hardware_id || !map.cpus[i].enabled)
            continue;
        map.cpus[i].boot = true;
        map.boot_index = i;
        map.boot_known = true;
        return true;
    }
    map.method = CpuStartMethod::none;
    map.up_reason = "boot-cpu-not-described";
    return false;
}

} // namespace ok::boot
