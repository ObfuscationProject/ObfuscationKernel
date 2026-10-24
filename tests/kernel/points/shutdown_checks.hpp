#pragma once

#include "ok/core/kernel.hpp"
#include "ok/core/shutdown.hpp"
#include "ok/core/native_shutdown.hpp"
#include "ok/syscall/syscall.hpp"
#include "ok/uapi/syscall.h"
#include "ok/user/native_power_profile.hpp"
#include "ok/user/runtime.hpp"
#include "ok/arch/pc8042_reset.hpp"
#include "ok/arch/piix4_pm_poweroff.hpp"

namespace ok::tests
{
struct ShutdownPc8042ModelPorts
{
    static inline usize reads{}, writes{};
    static u8 read_status() noexcept { return ++reads <= 3 ? 2 : 0; }
    static void write_command(u8 value) noexcept { if (value == 0xfe) ++writes; }
    static void pause() noexcept {}
};
// Reachable fake configuration/port model; no physical PM action is requested.
struct ShutdownPiix4ModelPorts
{
    static inline usize control_reads{}, writes{};
    static inline bool enabled{true};
    static u32 read_configuration(u8 slot, u8 function, u8 offset) noexcept
    {
        if (slot != 7 || (function != 0 && function != 3)) return 0xffffffffu;
        if (function == 0)
            return offset == 0 ? 0x70008086u : offset == 0x0c ? 0x00800000u : 0;
        switch (offset)
        {
        case 0x00: return 0x71138086u;
        case 0x08: return 0x06800003u;
        case 0x0c: return 0;
        case 0x40: return 0x00000601u;
        case 0x80: return enabled ? 1 : 0;
        default: return 0;
        }
    }
    static u16 read_control(u16 port) noexcept
    {
        ++control_reads;
        return port == 0x604 ? 1 : 0xffffu;
    }
    static void write_control(u16 port, u16 value) noexcept
    {
        if (port == 0x604 && value == 0x2001) ++writes;
    }
};
static_assert(OK_UAPI_VERSION_MAJOR == 0 && OK_UAPI_VERSION_MINOR == 16 && OK_UAPI_VERSION_PATCH == 0);
static_assert(OK_NATIVE_POWER_VERSION == user::native_power_version && OK_NATIVE_POWER_VERSION == 1);
static_assert(OK_POWER_TOKEN_MAX == user::native_power_token_limit && OK_POWER_TOKEN_MAX == 0x3fffffffU);
static_assert(OK_POWER_HALT == 1 && OK_POWER_REBOOT == 2 && OK_POWER_POWEROFF == 3);
static_assert(static_cast<u64>(syscall::Number::power_prepare) == OK_SYS_POWER_PREPARE && OK_SYS_POWER_PREPARE == 1032);
static_assert(static_cast<u64>(syscall::Number::power_commit) == OK_SYS_POWER_COMMIT && OK_SYS_POWER_COMMIT == 1033);
// Reachable standalone refusal only: there is no original native init at this
// boot point. This must never fabricate an owner or reserve/queue real shutdown.
inline Status check_native_power_wire_refusal_model(Kernel &kernel)
{
    using Ports = ShutdownPc8042ModelPorts;
    Ports::reads = Ports::writes = 0;
    if (!arch::detail::Pc8042Reset<Ports>::preflight() || Ports::reads != 4 || Ports::writes ||
        !arch::detail::Pc8042Reset<Ports>::request() || Ports::writes != 1)
        return Status::fault("PC/8042 fake-port policy model failed");
    using Pm = ShutdownPiix4ModelPorts;
    using Policy = arch::detail::Piix4PmPoweroff<Pm, Pm>;
    Pm::enabled = true;
    Pm::control_reads = Pm::writes = 0;
    auto pm = Policy::probe();
    if (!pm || pm.value().slot != 7 || pm.value().function != 3 || pm.value().control_port != 0x604 ||
        !Policy::preflight() || Pm::control_reads || Pm::writes || !Policy::request() ||
        Pm::control_reads != 2 || Pm::writes != 1)
        return Status::fault("PIIX4 PM fake-port discovery/request model failed");
    Pm::enabled = false;
    Pm::control_reads = Pm::writes = 0;
    if (Policy::request().code() != StatusCode::unsupported || Pm::control_reads || Pm::writes)
        return Status::fault("PIIX4 PM fake-port disabled-decode refusal failed");
    if (user::native::shutdown_owner(kernel) || native_shutdown_pending())
        return Status::fault("native power no-init model requires an unbound runtime");
    for (u32 action : {0U, OK_POWER_HALT, OK_POWER_REBOOT, OK_POWER_POWEROFF, 0xffffffffU})
        if (prepare_native_power(kernel, action).status().code() != StatusCode::not_initialized)
            return Status::fault("native power prepare bypassed original-init admission");
    for (u32 token : {0U, 1U, OK_POWER_TOKEN_MAX, OK_POWER_TOKEN_MAX + 1U, 0xffffffffU})
        if (commit_native_power(kernel, token).code() != StatusCode::not_initialized)
            return Status::fault("native power commit bypassed original-init admission");
    return !native_shutdown_pending() ? Status{} : Status::fault("no-init power refusal queued shutdown");
}
struct ShutdownRefusalServices
{
    Status preflight(SystemPowerAction) { return Status::unsupported("no shutdown adapter"); }
    Status check_commit(ShutdownOwner) { return Status::fault("unexpected commit"); }
    Status check_outer_boundary() { return Status::fault("unexpected boundary"); }
    Status close_admission() { return Status::fault("unexpected barrier"); }
    Status stop_producers() { return Status::fault("unexpected producer retirement"); }
    Status retire_native() { return Status::fault("unexpected native retirement"); }
    Status drain_resources() { return Status::fault("unexpected resource drain"); }
    Status seal_storage() { return Status::fault("unexpected storage seal"); }
};
// Reachable refusal/model check only, without retiring the running test kernel.
inline Status check_shutdown_refusal_model(Kernel &kernel)
{
    ShutdownRefusalServices services;
    ShutdownCoordinator coordinator{services};
    ShutdownOwner owner{&kernel, 1, 1, 1};
    ShutdownCaller caller{owner, 1, 1, true, {}};
    if (!coordinator.bind_original_init(owner) ||
        coordinator.prepare(caller, SystemPowerAction::halt).status().code() != StatusCode::unsupported ||
        coordinator.phase() != ShutdownPhase::idle || coordinator.must_hold() ||
        coordinator.ready_action() != SystemPowerAction::none)
        return Status::fault("shutdown refusal model failed");
    for (auto action : {SystemPowerAction::halt, SystemPowerAction::reboot, SystemPowerAction::poweroff})
        if (kernel.request_power_action(action).code() != StatusCode::unsupported ||
            kernel.power_action() != SystemPowerAction::none)
            return Status::fault("legacy power request bypassed shutdown refusal");
    for (auto action : {SystemPowerAction::none, static_cast<SystemPowerAction>(255)})
        if (kernel.request_power_action(action).code() != StatusCode::invalid_argument)
            return Status::fault("invalid power request was accepted");
    // Fresh scheduler: irreversible admission closure still permits owner drain.
    static sched::Scheduler scheduler;
    auto context = kernel.arch().make_kernel_context(0x1000, 0x8000);
    auto process = scheduler.create_process("shutdown-barrier-model", context);
    if (!process || !scheduler.set_runnable(process.value()))
        return Status::fault("shutdown scheduler fixture failed");
    if (!scheduler.close_shutdown_admission() || !scheduler.shutdown_admission_closed() ||
        scheduler.close_shutdown_admission().code() != StatusCode::denied ||
        scheduler.create_process("late", context).status().code() != StatusCode::denied ||
        scheduler.create_thread(process.value(), context).status().code() != StatusCode::denied ||
        scheduler.schedule_next().status().code() != StatusCode::denied ||
        scheduler.activate(process.value()).code() != StatusCode::denied ||
        scheduler.activate_thread(process.value(), 1).code() != StatusCode::denied ||
        !scheduler.activate(0) ||
        scheduler.allocate_description({}).status().code() != StatusCode::denied ||
        scheduler.prepare_descriptor(0).status().code() != StatusCode::denied ||
        !scheduler.drain_all_resources())
        return Status::fault("shutdown scheduler admission model failed");
    scheduler.boot_resources().prepared_descriptor_operations = 1;
    if (scheduler.drain_all_resources().code() != StatusCode::busy)
        return Status::fault("shutdown drain ignored an inconsistent retained operation");
    scheduler.boot_resources().prepared_descriptor_operations = 0;
    if (scheduler.drain_all_resources().code() != StatusCode::busy ||
        scheduler.resource_drain_report().first_failure.code() != StatusCode::busy)
        return Status::fault("shutdown drain forgot its first ownership failure");
    ModuleManager manager;
    if (!manager.close_shutdown_admission() ||
        manager.start_all().code() != StatusCode::denied ||
        manager.restart_module("gui").code() != StatusCode::denied)
        return Status::fault("shutdown module admission model failed");
    driver::DriverManager drivers;
    if (!drivers.close_shutdown_admission() ||
        drivers.start_all().code() != StatusCode::denied ||
        drivers.add(kernel.console()).code() != StatusCode::denied ||
        !drivers.stop_shutdown_producers() ||
        drivers.stop_shutdown_producers().code() != StatusCode::denied)
        return Status::fault("shutdown driver admission model failed");
    return {};
}
} // namespace ok::tests
