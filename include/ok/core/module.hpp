#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok
{

inline constexpr usize max_kernel_modules = 32;
inline constexpr usize max_kernel_services = 64;
inline constexpr usize max_module_name = 48;

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

struct ModuleManifest
{
    std::string_view name{};
    std::string_view version{};
    std::string_view module_class{};
    std::span<const ModuleDependency> dependencies{};
    std::span<const std::string_view> exported_services{};
    std::span<const std::string_view> required_services{};
    bool built_in{true};
    u32 init_priority{0};
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

    [[nodiscard]] ModuleState state() const
    {
        return state_;
    }
    [[nodiscard]] std::string_view failure_message() const
    {
        return failure_message_;
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
};

class ModuleManager final
{
  public:
    Status register_module(KernelModule &module);
    Status start_all();
    Status restart_module(std::string_view name);
    Status stop_all();
    Status shutdown_all();

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
    [[nodiscard]] ServiceRegistry &services()
    {
        return services_;
    }
    [[nodiscard]] const ServiceRegistry &services() const
    {
        return services_;
    }

  private:
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
    Status check_required_services(KernelModule &module) const;
    Status publish_services(KernelModule &module);
    Status transition(KernelModule &module, ModuleState next, Status status);
    [[nodiscard]] bool started_order_contains(const KernelModule &module) const;
    Status record_started(KernelModule &module);
    Status start_module(KernelModule &module);

    StaticVector<KernelModule *, max_kernel_modules> modules_;
    StaticVector<usize, max_kernel_modules> sorted_order_;
    StaticVector<KernelModule *, max_kernel_modules> started_order_;
    std::array<VisitState, max_kernel_modules> visit_state_{};
    ServiceRegistry services_;
};

[[nodiscard]] std::string_view module_state_name(ModuleState state);

} // namespace ok
