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
// How the started CPUs' clock counters agree with the boot CPU's, for the
// OK_SMP line's tsc= field (x86_64: sync, adjusted or offset, the worst over
// the online secondaries; start() fails for a CPU it could not verify), or
// empty where every CPU reads one counter or no secondary is online.
[[nodiscard]] std::string_view clock_sync_name() noexcept;
// Raise the IPI line of `cpu`; the reasons are already in its record.
void send_ipi(ok::smp::CpuId cpu, u64 hardware_id) noexcept;
// Secondary idle: wait for an IPI or this CPU's local timer, acknowledge it and
// report it through ok::smp::receive_ipi() or ok::smp::local_tick().
void wait_local() noexcept;

// Hotplug, on profiles that start secondaries (OK_ARCH_SMP_START).
// On the CPU leaving, holding nothing: stop its local timer and turn off its
// interrupt controller interface, so no tick or IPI is taken any more.
void stop_local() noexcept;
// On that CPU once its state reads offline: leave the kernel for good without
// touching its stack again. start() starts it again.
[[noreturn]] void park_self(ok::smp::CpuId cpu) noexcept;
// On the requesting CPU: `cpu` no longer runs (or never ran) and its stack and
// start block may be reused: x86 its parked flag, aarch64 PSCI AFFINITY_INFO
// OFF, rv64 its cleared start mailbox entry.
[[nodiscard]] bool parked(ok::smp::CpuId cpu, u64 hardware_id) noexcept;
// On the requesting CPU, for a CPU that start() launched but bring-up or a
// restart then gave up on (it never came online, or with another id): send
// it back to a parked state where the architecture can stop another CPU (x86:
// INIT, after which parked() is true and start() may run again). aarch64 and
// rv64 cannot; such a CPU idles outside the online mask and parked() stays
// false, so it is not restarted.
void abandon(ok::smp::CpuId cpu, u64 hardware_id) noexcept;

} // namespace ok::arch::smp
