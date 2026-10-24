#include "ok/core/shutdown.hpp"
namespace
{
struct CompileServices
{
    ok::Status preflight(ok::SystemPowerAction) { return {}; }
    ok::Status check_commit(ok::ShutdownOwner) { return {}; }
    ok::Status check_outer_boundary() { return {}; }
    ok::Status close_admission() { return {}; }
    ok::Status stop_producers() { return {}; }
    ok::Status retire_native() { return {}; }
    ok::Status drain_resources() { return {}; }
    ok::Status seal_storage() { return {}; }
};
}
// Compile-only instantiation, never a native retirement or storage seal test.
ok::Status compile_shutdown_paths()
{
    CompileServices services;
    ok::ShutdownCoordinator coordinator{services};
    ok::ShutdownOwner owner{&services, 1, 1, 1};
    ok::ShutdownCaller caller{owner, 1, 1, true, {}};
    if (auto status = coordinator.bind_original_init(owner); !status) return status;
    auto ticket = coordinator.prepare(caller, ok::SystemPowerAction::halt);
    if (!ticket) return ticket.status();
    if (auto status = coordinator.commit(caller, ticket.value()); !status) return status;
    if (auto status = coordinator.advance(); !status) return status;
    auto action = coordinator.consume_action();
    return action.status();
}
