#pragma once

#include "ok/boot/cpu_topology.hpp"
#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"

#include <string_view>

// Secondary-CPU operations of the configured architecture. Each profile links
// exactly one implementation (src/arch/<arch>/smp.cpp, or src/arch/smp_stub.cpp
// for profiles that run UP), like the execution backend.
namespace ok::arch::smp
{

// Fills `map` from the firmware's description of this machine's CPUs. A
// machine the profile cannot start secondaries on leaves one CPU and sets
// `up_reason`.
Status discover(boot::CpuTopologyMap &map);
// The method name printed in the OK_SMP line, e.g. "sipi".
[[nodiscard]] std::string_view start_method_name(const boot::CpuTopologyMap &map) noexcept;
// The boot CPU's part of bring-up, after execution::initialize(): enable the
// local interrupt controller interface used for IPIs.
Status prepare_boot_cpu(const boot::CpuTopologyMap &map);
// This CPU's identity as the firmware tables name it (APIC id, MPIDR affinity,
// hart id), read from its own registers.
[[nodiscard]] u64 current_hardware_id() noexcept;
// Start logical CPU `cpu` with firmware identity `hardware_id`. It runs the
// architecture's secondary entry on [stack_lower, stack_top) and then calls
// ok_smp_secondary_main(cpu). Returns once the start request was issued.
Status start(ok::smp::CpuId cpu, u64 hardware_id, uptr stack_lower, uptr stack_top);
// Raise the IPI line of `cpu`; the reasons are already in its record.
void send_ipi(ok::smp::CpuId cpu, u64 hardware_id) noexcept;
// Secondary idle: wait for an IPI or this CPU's local timer, acknowledge it and
// report it through ok::smp::receive_ipi() or ok::smp::local_tick().
void wait_local() noexcept;

} // namespace ok::arch::smp
