#include "ok/core/native_shutdown.hpp"
#include "ok/core/kernel.hpp"
#include "ok/core/shutdown_storage.hpp"
#include "ok/arch/terminal_power.hpp"
#include "ok/user/runtime.hpp"
#include "ok/user/native_power_profile.hpp"
#include "ok/uapi/syscall.h"

extern "C" ok::Status ok_shutdown_storage_preflight(ok::fs::VirtualFileSystem &,
    ok::SystemPowerAction) __attribute__((weak));
extern "C" ok::Status ok_shutdown_storage_seal(ok::fs::VirtualFileSystem &,
    const ok::ShutdownStorageBarrier &) __attribute__((weak));

namespace ok::user
{
class NativeShutdownServices final
{
  public:
    Kernel *kernel{};
    ShutdownOwner owner{};
    ShutdownTicket ticket{};
    u32 power_token{};
    const NativeShutdownBoundary *boundary{};

    Status preflight(SystemPowerAction action)
    {
        if (!kernel)
            return Status::unsupported("native shutdown action has no qualified backend");
        if (auto status = arch::preflight_terminal_power_action(action); !status) return status;
        if (!ok_shutdown_storage_preflight || !ok_shutdown_storage_seal)
            return Status::unsupported("native shutdown has no original storage service");
        if (auto status = native::shutdown_preflight(*kernel, owner); !status) return status;
        if (native_shutdown_pending())
            if (auto status = native::shutdown_check_commit(*kernel, owner); !status) return status;
        if (auto status = kernel->shutdown_preflight_producers(); !status) return status;
        return ok_shutdown_storage_preflight(kernel->vfs(), action);
    }
    Status check_commit(ShutdownOwner original)
    {
        return kernel && original == owner ? native::shutdown_check_commit(*kernel, owner)
            : Status::denied("shutdown runtime owner differs");
    }
    Status check_outer_boundary()
    {
        if (!boundary || !kernel) return Status::denied("shutdown is outside the desktop outer boundary");
        return native::shutdown_outer_boundary(*kernel);
    }
    Status close_admission()
    {
        if (auto status = check_outer_boundary(); !status) return status;
        if (auto status = native::shutdown_close_admission(*kernel); !status) return status;
        return kernel->shutdown_close_admission();
    }
    Status stop_producers() { return kernel->shutdown_stop_producers(); }
    Status retire_native() { return native::shutdown_retire(*kernel); }
    Status drain_resources()
    {
        if (auto status = kernel->scheduler().drain_all_resources(); !status) return status;
        if (auto status = native::shutdown_verify_drained(*kernel); !status) return status;
        if (auto status = kernel->shutdown_verify_producers_stopped(); !status) return status;
        return kernel->vfs().shutdown_reference_status();
    }
    Status seal_storage()
    {
        if (auto status = check_outer_boundary(); !status) return status;
        if (auto status = native::shutdown_verify_drained(*kernel); !status) return status;
        if (auto status = kernel->shutdown_verify_producers_stopped(); !status) return status;
        if (auto status = kernel->vfs().shutdown_reference_status(); !status) return status;
        const ShutdownStorageBarrier barrier{ticket};
        return ok_shutdown_storage_seal(kernel->vfs(), barrier);
    }
};
} // namespace ok::user

namespace ok
{
namespace
{
// Freestanding entry does not run hosted dynamic initialization. Require the
// service reference and all admission state to exist in the linked image.
constinit user::NativeShutdownServices services{};
constinit ShutdownCoordinator<user::NativeShutdownServices> coordinator{services};
}

Result<ShutdownTicket> prepare_native_shutdown(Kernel &kernel, SystemPowerAction action)
{
    auto caller = user::native::shutdown_caller(kernel);
    if (!caller) return caller.status();
    if (!services.kernel)
    {
        if (auto status = coordinator.bind_original_init(caller.value().owner); !status) return status;
        services.kernel = &kernel;
        services.owner = caller.value().owner;
    }
    if (services.kernel != &kernel) return Status::denied("shutdown kernel instance differs");
    auto prepared = coordinator.prepare(caller.value(), action);
    if (prepared) services.ticket = prepared.value();
    return prepared;
}
Status commit_native_shutdown(Kernel &kernel, ShutdownTicket ticket)
{
    if (services.kernel != &kernel) return Status::denied("shutdown kernel instance differs");
    auto caller = user::native::shutdown_caller(kernel);
    return caller ? coordinator.commit(caller.value(), ticket) : caller.status();
}
Result<u32> prepare_native_power(Kernel &kernel, u32 action)
{
    // Authenticate even a repeated or malformed request before inspecting the
    // boot-local selector. The token is neither a secret nor caller identity.
    auto caller = user::native::shutdown_caller(kernel);
    if (!caller) return caller.status();
    SystemPowerAction selected{};
    switch (action)
    {
    case OK_POWER_HALT: selected = SystemPowerAction::halt; break;
    case OK_POWER_REBOOT: selected = SystemPowerAction::reboot; break;
    case OK_POWER_POWEROFF: selected = SystemPowerAction::poweroff; break;
    default: return Status::invalid_argument("unknown native power action");
    }
    auto prepared = prepare_native_shutdown(kernel, selected);
    if (!prepared) return prepared.status();
    // The coordinator permits one reservation for one boot lifetime, without
    // cancellation or reset. Its exact pointer-bearing ticket remains private.
    static_assert(OK_POWER_TOKEN_MAX == user::native_power_token_limit);
    services.power_token = 1;
    return services.power_token;
}
Status commit_native_power(Kernel &kernel, u32 token)
{
    auto caller = user::native::shutdown_caller(kernel);
    if (!caller) return caller.status();
    if (!token || token > user::native_power_token_limit || !services.power_token ||
        token != services.power_token)
        return Status::invalid_argument("native power token is not the original reservation");
    return commit_native_shutdown(kernel, services.ticket);
}
bool native_shutdown_pending() { return coordinator.must_hold(); }
Result<SystemPowerAction> advance_native_shutdown(Kernel &kernel, const NativeShutdownBoundary &boundary)
{
    if (!coordinator.must_hold()) return SystemPowerAction::none;
    if (services.kernel != &kernel) return Status::denied("shutdown kernel instance differs");
    struct Scope
    {
        user::NativeShutdownServices &services;
        explicit Scope(user::NativeShutdownServices &value, const NativeShutdownBoundary &token) : services(value)
        { services.boundary = &token; }
        ~Scope() { services.boundary = nullptr; }
    } scope{services, boundary};
    if (auto status = coordinator.advance(); !status) return status;
    return coordinator.consume_action();
}
} // namespace ok
