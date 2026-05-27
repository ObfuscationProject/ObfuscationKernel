#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/sched/scheduler.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok
{

inline constexpr usize max_kernel_modules = 32;
inline constexpr usize max_kernel_services = 64;
inline constexpr usize max_module_name = 48;
inline constexpr std::string_view kernel_module_process_prefix{"mod:"};
inline constexpr u32 kernel_module_abi_version = 1;

struct ModuleId
{
    FixedString<max_module_name> name{};

    ModuleId() = default;
    explicit ModuleId(std::string_view value)
    {
        static_cast<void>(name.assign(value));
    }

    [[nodiscard]] std::string_view view() const
    {
        return name.view();
    }
};

struct ModuleDependency
{
    std::string_view name{};
    bool required{true};
};

enum class ModuleState : u8
{
    created,
    probed,
    initialized,
    started,
    stopped,
    failed,
};

enum class ModuleExecution : u8
{
    inline_core,
    kernel_process,
};

enum class ModuleThreading : u8
{
    single_threaded,
    per_cpu,
};

enum class ModuleTrustLevel : u8
{
    kernel,
    privileged,
    sandboxed,
};

enum class ModuleRestartPolicy : u8
{
    never,
    manual,
    on_failure,
    always,
};

enum class ModuleCapability : u8
{
    exports_services,
    requires_services,
    owns_kernel_process,
    uses_per_cpu_workers,
    handles_user_abi,
    handles_driver_abi,
    handles_gui,
};

struct ModuleResourceBudget
{
    usize max_threads{1};
    usize max_services{4};
    usize max_memory_pages{0};
    usize max_handles{0};
};

[[nodiscard]] constexpr u64 module_capability_bit(ModuleCapability capability)
{
    return 1ull << static_cast<u8>(capability);
}

struct ModuleManifest
{
    std::string_view name{};
    std::string_view version{};
    std::string_view module_class{};
    std::span<const ModuleDependency> dependencies{};
    std::span<const std::string_view> exported_services{};
    std::span<const std::string_view> required_services{};
    bool built_in{true};
    ModuleExecution execution{ModuleExecution::inline_core};
    u32 init_priority{0};
    ModuleThreading threading{ModuleThreading::single_threaded};
    u32 abi_version{kernel_module_abi_version};
    u64 capability_mask{0};
    ModuleTrustLevel trust{ModuleTrustLevel::kernel};
    ModuleRestartPolicy restart_policy{ModuleRestartPolicy::manual};
    ModuleResourceBudget resources{};
};

class KernelService
{
  public:
    virtual ~KernelService() = default;
    [[nodiscard]] virtual std::string_view service_id() const = 0;
};

class ServiceRegistry final
{
  public:
    Status register_service(std::string_view service_id, void *service);
    [[nodiscard]] void *query_raw(std::string_view service_id) const;
    [[nodiscard]] bool contains(std::string_view service_id) const
    {
        return query_raw(service_id) != nullptr;
    }
    [[nodiscard]] usize service_count() const
    {
        return services_.size();
    }

    template <typename T> [[nodiscard]] T *query(std::string_view service_id) const
    {
        return static_cast<T *>(query_raw(service_id));
    }

  private:
    struct Entry
    {
        std::string_view service_id{};
        void *service{nullptr};
    };

    StaticVector<Entry, max_kernel_services> services_;
};

class KernelModule
{
  public:
    virtual ~KernelModule() = default;
    [[nodiscard]] virtual ModuleManifest manifest() const = 0;
    virtual Status probe()
    {
        return Status::success();
    }
    virtual Status init(ServiceRegistry &)
    {
        return Status::success();
    }
    virtual Status start(ServiceRegistry &)
    {
        return Status::success();
    }
    virtual Status stop()
    {
        return Status::success();
    }
    virtual Status shutdown()
    {
        return Status::success();
    }
    [[nodiscard]] virtual void *service(std::string_view)
    {
        return this;
    }

    [[nodiscard]] ModuleState state() const
    {
        return state_;
    }
    [[nodiscard]] std::string_view failure_message() const
    {
        return failure_message_;
    }
    [[nodiscard]] usize restart_count() const
    {
        return restart_count_;
    }

  private:
    friend class ModuleManager;

    void set_state(ModuleState state)
    {
        state_ = state;
    }
    void fail(std::string_view message)
    {
        state_ = ModuleState::failed;
        failure_message_ = message;
    }
    void clear_failure()
    {
        failure_message_ = {};
    }

    ModuleState state_{ModuleState::created};
    std::string_view failure_message_{};
    usize restart_count_{0};
};

class ModuleManager final
{
  public:
    struct ModuleProcessRestart
    {
        FixedString<sched::max_process_name> process_name{};
        sched::ProcessId previous_pid{0};
        sched::ProcessId pid{0};
    };

    Status register_module(KernelModule &module);
    Status bind_kernel_process(sched::Scheduler &scheduler, arch::ArchOperations &arch, uptr entry, uptr stack);
    Status start_all();
    Status restart_module(std::string_view name);
    Status supervise_kernel_processes(StaticVector<ModuleProcessRestart, max_kernel_modules> &restarts);
    Status stop_all();
    Status shutdown_all();
    Result<sched::ProcessId> ensure_kernel_process();

    [[nodiscard]] KernelModule *find(std::string_view name) const;
    [[nodiscard]] bool has_module(std::string_view name) const
    {
        return find(name) != nullptr;
    }
    [[nodiscard]] usize module_count() const
    {
        return modules_.size();
    }
    [[nodiscard]] usize failed_count() const;
    [[nodiscard]] usize started_count() const
    {
        return started_order_.size();
    }
    [[nodiscard]] sched::ProcessId kernel_process_pid() const
    {
        return kernel_process_pid_;
    }
    [[nodiscard]] usize kernel_process_module_count() const
    {
        return kernel_process_modules_;
    }
    [[nodiscard]] ServiceRegistry &services()
    {
        return services_;
    }
    [[nodiscard]] const ServiceRegistry &services() const
    {
        return services_;
    }

  private:
    struct ModuleProcessRecord
    {
        FixedString<max_module_name> module_name{};
        sched::ProcessId pid{0};
    };

    enum class VisitState : u8
    {
        unvisited,
        visiting,
        visited,
    };

    [[nodiscard]] Result<usize> index_of(std::string_view name) const;
    Status sort_modules();
    Status visit(usize index);
    Status check_dependencies(KernelModule &module) const;
    Status validate_manifest(KernelModule &module) const;
    Status check_required_services(KernelModule &module) const;
    Status publish_services(KernelModule &module);
    Status transition(KernelModule &module, ModuleState next, Status status);
    [[nodiscard]] bool started_order_contains(const KernelModule &module) const;
    Status record_started(KernelModule &module);
    Status start_module(KernelModule &module);
    Result<sched::ProcessId> ensure_kernel_process(KernelModule &module);
    Status ensure_kernel_process_threads(sched::ProcessId pid, ModuleThreading threading, usize slot);

    StaticVector<KernelModule *, max_kernel_modules> modules_;
    StaticVector<usize, max_kernel_modules> sorted_order_;
    StaticVector<KernelModule *, max_kernel_modules> started_order_;
    StaticVector<ModuleProcessRecord, max_kernel_modules> module_processes_;
    std::array<VisitState, max_kernel_modules> visit_state_{};
    ServiceRegistry services_;
    sched::Scheduler *kernel_process_scheduler_{nullptr};
    arch::ArchOperations *kernel_process_arch_{nullptr};
    sched::ProcessId kernel_process_pid_{0};
    uptr kernel_process_entry_{0};
    uptr kernel_process_stack_{0};
    usize kernel_process_modules_{0};
};

[[nodiscard]] std::string_view module_state_name(ModuleState state);
[[nodiscard]] std::string_view module_execution_name(ModuleExecution execution);

} // namespace ok
