#include "roadmap_tests.hpp"

#include "ok/core/module.hpp"

#include <array>
#include <span>

namespace ok
{
namespace
{

class DebugModule final : public KernelModule
{
  public:
    DebugModule(std::string_view name, std::string_view klass, std::span<const ModuleDependency> dependencies,
                std::span<const std::string_view> exports, std::span<const std::string_view> required_services,
                u32 priority, usize *start_sequence = nullptr, usize *stop_sequence = nullptr,
                ModuleExecution execution = ModuleExecution::inline_core)
        : name_(name), klass_(klass), dependencies_(dependencies), exports_(exports), requires_(required_services),
          priority_(priority), start_sequence_(start_sequence), stop_sequence_(stop_sequence), execution_(execution)
    {
    }

    [[nodiscard]] ModuleManifest manifest() const override
    {
        return ModuleManifest{
            .name = name_.view(),
            .version = "1",
            .module_class = klass_.view(),
            .dependencies = dependencies_,
            .exported_services = exports_,
            .required_services = requires_,
            .built_in = true,
            .execution = execution_,
            .init_priority = priority_,
        };
    }

    Status start(ServiceRegistry &) override
    {
        if (start_sequence_ != nullptr)
        {
            started_at_ = ++(*start_sequence_);
        }
        return Status::success();
    }

    Status stop() override
    {
        if (stop_sequence_ != nullptr)
        {
            stopped_at_ = ++(*stop_sequence_);
        }
        return Status::success();
    }

    [[nodiscard]] usize started_at() const
    {
        return started_at_;
    }
    [[nodiscard]] usize stopped_at() const
    {
        return stopped_at_;
    }

  private:
    FixedString<max_module_name> name_{};
    FixedString<max_module_name> klass_{};
    std::span<const ModuleDependency> dependencies_{};
    std::span<const std::string_view> exports_{};
    std::span<const std::string_view> requires_{};
    u32 priority_{0};
    usize *start_sequence_{nullptr};
    usize *stop_sequence_{nullptr};
    ModuleExecution execution_{ModuleExecution::inline_core};
    usize started_at_{0};
    usize stopped_at_{0};
};

Status test_single_module()
{
    DebugModule module{"standalone", "test", {}, {}, {}, 0};
    ModuleManager manager;
    if (auto status = manager.register_module(module); !status.ok())
    {
        return status;
    }
    if (auto status = manager.start_all(); !status.ok())
    {
        return status;
    }
    if (manager.module_count() != 1 || manager.started_count() != 1 || module.state() != ModuleState::started)
    {
        return Status::fault("single module lifecycle validation failed");
    }
    if (auto status = manager.stop_all(); !status.ok())
    {
        return status;
    }
    return module.state() == ModuleState::stopped ? Status::success()
                                                  : Status::fault("single module stop validation failed");
}

Status test_missing_dependency()
{
    constexpr std::array<ModuleDependency, 1> missing_dep{ModuleDependency{.name = "missing", .required = true}};
    DebugModule missing{"needs-missing", "test", missing_dep, {}, {}, 0};
    ModuleManager manager;
    if (auto status = manager.register_module(missing); !status.ok())
    {
        return status;
    }
    if (manager.start_all().code() != StatusCode::not_found || missing.state() != ModuleState::failed)
    {
        return Status::fault("missing module dependency was not rejected");
    }
    return Status::success();
}

Status test_dependency_cycle()
{
    constexpr std::array<ModuleDependency, 1> dep_b{ModuleDependency{.name = "cycle-b", .required = true}};
    constexpr std::array<ModuleDependency, 1> dep_a{ModuleDependency{.name = "cycle-a", .required = true}};
    DebugModule cycle_a{"cycle-a", "test", dep_b, {}, {}, 0};
    DebugModule cycle_b{"cycle-b", "test", dep_a, {}, {}, 0};
    ModuleManager manager;
    static_cast<void>(manager.register_module(cycle_a));
    static_cast<void>(manager.register_module(cycle_b));
    if (manager.start_all().code() != StatusCode::invalid_argument || manager.failed_count() == 0)
    {
        return Status::fault("module dependency cycle was not rejected");
    }
    return Status::success();
}

Status test_missing_required_service()
{
    constexpr std::array<std::string_view, 1> required_service{"missing.service"};
    DebugModule module{"needs-service", "test", {}, {}, required_service, 0};
    ModuleManager manager;
    if (auto status = manager.register_module(module); !status.ok())
    {
        return status;
    }
    if (manager.start_all().code() != StatusCode::not_found || module.state() != ModuleState::failed)
    {
        return Status::fault("missing required service was not rejected");
    }
    return Status::success();
}

Status test_duplicate_service()
{
    constexpr std::array<std::string_view, 1> exports{"duplicate.service"};
    DebugModule first{"service-a", "test", {}, exports, {}, 0};
    DebugModule second{"service-b", "test", {}, exports, {}, 10};
    ModuleManager manager;
    static_cast<void>(manager.register_module(first));
    static_cast<void>(manager.register_module(second));
    if (manager.start_all().code() != StatusCode::already_exists || second.state() != ModuleState::failed ||
        manager.services().query_raw("duplicate.service") != &first)
    {
        return Status::fault("duplicate service was not rejected");
    }
    return Status::success();
}

Status test_dependency_priority_order()
{
    usize start_sequence = 0;
    constexpr std::array<ModuleDependency, 1> depends_provider{ModuleDependency{.name = "provider", .required = true}};
    DebugModule dependent{"dependent", "test", depends_provider, {}, {}, 0, &start_sequence};
    DebugModule provider{"provider", "test", {}, {}, {}, 100, &start_sequence};
    ModuleManager manager;
    static_cast<void>(manager.register_module(dependent));
    static_cast<void>(manager.register_module(provider));
    if (auto status = manager.start_all(); !status.ok())
    {
        return status;
    }
    if (provider.started_at() == 0 || dependent.started_at() == 0 || provider.started_at() >= dependent.started_at())
    {
        return Status::fault("module init priority overrode dependency order");
    }
    return Status::success();
}

Status test_kernel_process_backed_module()
{
    auto &ops = arch::arch_operations(arch::configured_architecture());
    sched::Scheduler scheduler;
    if (auto status = scheduler.configure_cpus(1); !status.ok())
    {
        return status;
    }

    DebugModule module{"gui-worker", "gui", {}, {}, {}, 0, nullptr, nullptr, ModuleExecution::kernel_process};
    ModuleManager manager;
    if (auto status = manager.register_module(module); !status.ok())
    {
        return status;
    }
    if (manager.start_all().code() != StatusCode::not_initialized || module.state() != ModuleState::failed)
    {
        return Status::fault("kernel-process module started without a module process binding");
    }

    ModuleManager bound_manager;
    DebugModule bound_module{"gui-worker", "gui", {}, {}, {}, 0, nullptr, nullptr, ModuleExecution::kernel_process};
    if (auto status = bound_manager.bind_kernel_process(scheduler, ops, 0x3000, 0xa000); !status.ok())
    {
        return status;
    }
    if (auto status = bound_manager.register_module(bound_module); !status.ok())
    {
        return status;
    }
    if (auto status = bound_manager.start_all(); !status.ok())
    {
        return status;
    }
    const auto pid = bound_manager.kernel_process_pid();
    auto *process = scheduler.find(pid);
    if (pid == 0 || process == nullptr || process->name() != "mod:gui-worker" || !process->background() ||
        bound_manager.kernel_process_module_count() != 1 || scheduler.background_process_count() != 1 ||
        bound_module.state() != ModuleState::started ||
        module_execution_name(bound_module.manifest().execution) != "kernel-process")
    {
        return Status::fault("kernel-process module manager binding failed");
    }
    if (auto status = bound_manager.restart_module("gui-worker"); !status.ok())
    {
        return status;
    }
    if (bound_manager.kernel_process_pid() != pid || bound_manager.kernel_process_module_count() != 1)
    {
        return Status::fault("kernel-process module restart did not reuse module process");
    }
    return Status::success();
}

Status test_builtin_module_graph(KernelTestReport &report)
{
    usize start_sequence = 0;
    usize stop_sequence = 0;
    constexpr std::array<std::string_view, 1> arch_exports{"arch.ops"};
    constexpr std::array<std::string_view, 1> memory_exports{"memory.frames"};
    constexpr std::array<std::string_view, 1> vfs_exports{"vfs.root"};
    constexpr std::array<std::string_view, 1> required_arch{"arch.ops"};
    constexpr std::array<ModuleDependency, 1> depends_arch{ModuleDependency{.name = "arch", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_interrupt{
        ModuleDependency{.name = "interrupt", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_scheduler{
        ModuleDependency{.name = "scheduler", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_driver{ModuleDependency{.name = "driver-core", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_vfs{ModuleDependency{.name = "vfs", .required = true}};
    constexpr std::array<ModuleDependency, 2> depends_vfs_and_syscall{
        ModuleDependency{.name = "vfs", .required = true},
        ModuleDependency{.name = "syscall", .required = true},
    };

    DebugModule arch_module{"arch", "architecture", {}, arch_exports, {}, 0, &start_sequence, &stop_sequence};
    DebugModule memory_module{"memory",      "memory", depends_arch,    memory_exports,
                              required_arch, 10,       &start_sequence, &stop_sequence};
    DebugModule interrupt_module{"interrupt", "interrupt", depends_arch, {}, {}, 20, &start_sequence, &stop_sequence};
    DebugModule scheduler_module{"scheduler", "scheduler", depends_interrupt, {},
                                 {},          30,          &start_sequence,   &stop_sequence};
    DebugModule smp_module{"smp", "smp", depends_scheduler, {}, {}, 40, &start_sequence, &stop_sequence};
    DebugModule ipc_module{"ipc", "ipc", depends_scheduler, {}, {}, 50, &start_sequence, &stop_sequence};
    DebugModule syscall_module{"syscall", "syscall", depends_scheduler, {}, {}, 60, &start_sequence, &stop_sequence};
    DebugModule driver_module{"driver-core", "driver", depends_interrupt, {}, {}, 70, &start_sequence, &stop_sequence};
    DebugModule vfs_module{"vfs", "filesystem", depends_driver, vfs_exports, {}, 80, &start_sequence, &stop_sequence};
    DebugModule posix_module{"posix", "posix", depends_vfs_and_syscall, {}, {}, 90, &start_sequence, &stop_sequence};
    DebugModule user_module{"user-mode", "user", depends_scheduler, {}, {}, 100, &start_sequence, &stop_sequence};
    DebugModule shell_module{"debug-shell", "shell", depends_vfs, {}, {}, 110, &start_sequence, &stop_sequence};

    ModuleManager manager;
    for (auto *module : {&arch_module, &memory_module, &interrupt_module, &scheduler_module, &smp_module, &ipc_module,
                         &syscall_module, &driver_module, &vfs_module, &posix_module, &user_module, &shell_module})
    {
        if (auto status = manager.register_module(*module); !status.ok())
        {
            return status;
        }
    }
    if (manager.register_module(arch_module).code() != StatusCode::already_exists)
    {
        return Status::fault("duplicate module name was not rejected");
    }
    if (auto status = manager.start_all(); !status.ok())
    {
        return status;
    }
    if (manager.failed_count() != 0 || manager.module_count() != 12 || manager.started_count() != 12 ||
        manager.services().query<DebugModule>("memory.frames") != &memory_module ||
        manager.services().query_raw("missing.service") != nullptr ||
        arch_module.started_at() >= memory_module.started_at() ||
        arch_module.started_at() >= interrupt_module.started_at() ||
        interrupt_module.started_at() >= scheduler_module.started_at() ||
        scheduler_module.started_at() >= smp_module.started_at() ||
        scheduler_module.started_at() >= ipc_module.started_at() ||
        scheduler_module.started_at() >= syscall_module.started_at() ||
        interrupt_module.started_at() >= driver_module.started_at() ||
        driver_module.started_at() >= vfs_module.started_at() || vfs_module.started_at() >= posix_module.started_at() ||
        syscall_module.started_at() >= posix_module.started_at() ||
        scheduler_module.started_at() >= user_module.started_at() ||
        vfs_module.started_at() >= shell_module.started_at())
    {
        return Status::fault("module manager dependency or service validation failed");
    }
    if (auto status = manager.stop_all(); !status.ok())
    {
        return status;
    }
    if (memory_module.stopped_at() >= arch_module.stopped_at() ||
        scheduler_module.stopped_at() >= interrupt_module.stopped_at() ||
        vfs_module.stopped_at() >= driver_module.stopped_at() || posix_module.stopped_at() >= vfs_module.stopped_at() ||
        shell_module.stopped_at() >= vfs_module.stopped_at())
    {
        return Status::fault("module manager stop order is not reverse dependency order");
    }

    report.modules = true;
    report.module_count = manager.module_count();
    report.module_failed_count = manager.failed_count();
    return Status::success();
}

} // namespace

Status run_module_roadmap_tests(KernelTestReport &report)
{
    if (auto status = test_single_module(); !status.ok())
    {
        return status;
    }
    if (auto status = test_missing_dependency(); !status.ok())
    {
        return status;
    }
    if (auto status = test_dependency_cycle(); !status.ok())
    {
        return status;
    }
    if (auto status = test_missing_required_service(); !status.ok())
    {
        return status;
    }
    if (auto status = test_duplicate_service(); !status.ok())
    {
        return status;
    }
    if (auto status = test_dependency_priority_order(); !status.ok())
    {
        return status;
    }
    if (auto status = test_kernel_process_backed_module(); !status.ok())
    {
        return status;
    }
    return test_builtin_module_graph(report);
}

} // namespace ok
