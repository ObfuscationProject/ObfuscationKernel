#include "ok/core/module.hpp"

namespace ok
{
namespace
{

constexpr uptr module_entry_stride = 0x100;
constexpr uptr module_stack_stride = 0x1000;
constexpr uptr module_thread_entry_stride = 0x10;
constexpr uptr module_thread_stack_stride = 0x100;

Status assign_module_process_name(FixedString<sched::max_process_name> &out, std::string_view module_name)
{
    if (auto status = out.assign(kernel_module_process_prefix); !status.ok())
    {
        return status;
    }
    constexpr usize max_chars = sched::max_process_name - 1;
    const auto room = out.size() < max_chars ? max_chars - out.size() : 0;
    return out.append(module_name.substr(0, room));
}

usize module_thread_target(ModuleThreading threading, usize cpu_count)
{
    if (threading != ModuleThreading::per_cpu)
    {
        return 1;
    }
    if (cpu_count == 0)
    {
        return 1;
    }
    return cpu_count < sched::max_threads_per_process ? cpu_count : sched::max_threads_per_process;
}

} // namespace

Status ServiceRegistry::register_service(std::string_view service_id, void *service)
{
    if (service_id.empty() || service == nullptr)
    {
        return Status::invalid_argument("invalid service registration");
    }
    for (const auto &entry : services_)
    {
        if (entry.service_id == service_id)
        {
            return Status::already_exists("service already registered");
        }
    }
    return services_.push_back(Entry{.service_id = service_id, .service = service});
}

void *ServiceRegistry::query_raw(std::string_view service_id) const
{
    for (const auto &entry : services_)
    {
        if (entry.service_id == service_id)
        {
            return entry.service;
        }
    }
    return nullptr;
}

Status ModuleManager::register_module(KernelModule &module)
{
    const auto manifest = module.manifest();
    if (manifest.name.empty())
    {
        return Status::invalid_argument("module name is empty");
    }
    if (has_module(manifest.name))
    {
        return Status::already_exists("module already registered");
    }
    if (auto status = validate_manifest(module); !status.ok())
    {
        module.fail(status.message());
        return status;
    }
    module.set_state(ModuleState::created);
    return modules_.push_back(&module);
}

Status ModuleManager::bind_kernel_process(sched::Scheduler &scheduler, arch::ArchOperations &arch, uptr entry,
                                          uptr stack)
{
    if (entry == 0 || stack == 0)
    {
        return Status::invalid_argument("kernel module process context is invalid");
    }
    kernel_process_scheduler_ = &scheduler;
    kernel_process_arch_ = &arch;
    kernel_process_entry_ = entry;
    kernel_process_stack_ = stack;
    kernel_process_pid_ = 0;
    kernel_process_modules_ = 0;
    module_processes_.clear();
    return Status::success();
}

Result<sched::ProcessId> ModuleManager::ensure_kernel_process()
{
    for (const auto &record : module_processes_)
    {
        if (kernel_process_scheduler_ != nullptr && kernel_process_scheduler_->find(record.pid) != nullptr)
        {
            return record.pid;
        }
    }
    return Status::not_initialized("kernel module process requires a module manifest");
}

Result<sched::ProcessId> ModuleManager::ensure_kernel_process(KernelModule &module)
{
    const auto manifest = module.manifest();
    ModuleProcessRecord *existing_record = nullptr;
    usize slot = module_processes_.size();
    for (usize i = 0; i < module_processes_.size(); ++i)
    {
        auto &record = module_processes_[i];
        if (record.module_name.view() != manifest.name)
        {
            continue;
        }
        existing_record = &record;
        slot = i;
        if (kernel_process_scheduler_ != nullptr && kernel_process_scheduler_->find(record.pid) != nullptr)
        {
            if (auto status = ensure_kernel_process_threads(record.pid, manifest.threading, slot); !status.ok())
            {
                module.fail(status.message());
                return status;
            }
            kernel_process_pid_ = record.pid;
            return record.pid;
        }
        break;
    }

    if (kernel_process_scheduler_ == nullptr || kernel_process_arch_ == nullptr)
    {
        return Status::not_initialized("kernel module process is not bound to a scheduler");
    }

    FixedString<sched::max_process_name> process_name;
    if (auto status = assign_module_process_name(process_name, manifest.name); !status.ok())
    {
        return status;
    }
    ModuleProcessRecord new_record{};
    if (existing_record == nullptr)
    {
        if (module_processes_.full())
        {
            return Status::overflow("kernel module process table is full");
        }
        if (auto status = new_record.module_name.assign(manifest.name); !status.ok())
        {
            return status;
        }
    }
    const auto context = kernel_process_arch_->make_kernel_context(
        kernel_process_entry_ + static_cast<uptr>(slot) * module_entry_stride,
        kernel_process_stack_ + static_cast<uptr>(slot) * module_stack_stride);
    auto process = kernel_process_scheduler_->spawn(sched::ScheduleRequest{
        .name = process_name.view(),
        .initial_context = context,
        .priority = sched::scheduler_default_priority,
        .cpu_affinity_mask = sched::cpu_affinity_any,
        .credentials = user::kernel_credentials(),
        .background = true,
        .cpu_accounting = sched::ProcessCpuAccounting::passive,
    });
    if (!process)
    {
        return process.status();
    }
    if (auto status = ensure_kernel_process_threads(process.value(), manifest.threading, slot); !status.ok())
    {
        static_cast<void>(kernel_process_scheduler_->kill_process(process.value()));
        module.fail(status.message());
        return status;
    }

    if (existing_record != nullptr)
    {
        existing_record->pid = process.value();
        kernel_process_pid_ = existing_record->pid;
        return existing_record->pid;
    }

    new_record.pid = process.value();
    if (auto status = module_processes_.push_back(new_record); !status.ok())
    {
        return status;
    }
    kernel_process_pid_ = process.value();
    return process.value();
}

Status ModuleManager::ensure_kernel_process_threads(sched::ProcessId pid, ModuleThreading threading, usize slot)
{
    if (kernel_process_scheduler_ == nullptr || kernel_process_arch_ == nullptr)
    {
        return Status::not_initialized("kernel module process is not bound to a scheduler");
    }
    auto *process = kernel_process_scheduler_->find(pid);
    if (process == nullptr)
    {
        return Status::not_found("kernel module process not found");
    }

    const auto target = module_thread_target(threading, kernel_process_scheduler_->cpu_count());
    while (process->threads().size() < target)
    {
        const auto worker = process->threads().size();
        const auto context = kernel_process_arch_->make_kernel_context(
            kernel_process_entry_ + static_cast<uptr>(slot) * module_entry_stride +
                static_cast<uptr>(worker) * module_thread_entry_stride,
            kernel_process_stack_ + static_cast<uptr>(slot) * module_stack_stride +
                static_cast<uptr>(worker) * module_thread_stack_stride);
        auto thread = kernel_process_scheduler_->create_thread(pid, context);
        if (!thread)
        {
            return thread.status();
        }
        process = kernel_process_scheduler_->find(pid);
        if (process == nullptr)
        {
            return Status::not_found("kernel module process disappeared");
        }
    }
    return Status::success();
}

KernelModule *ModuleManager::find(std::string_view name) const
{
    for (auto *module : modules_)
    {
        if (module->manifest().name == name)
        {
            return module;
        }
    }
    return nullptr;
}

usize ModuleManager::failed_count() const
{
    usize count = 0;
    for (auto *module : modules_)
    {
        if (module->state() == ModuleState::failed)
        {
            ++count;
        }
    }
    return count;
}

Result<usize> ModuleManager::index_of(std::string_view name) const
{
    for (usize i = 0; i < modules_.size(); ++i)
    {
        if (modules_[i]->manifest().name == name)
        {
            return i;
        }
    }
    return Status::not_found("module dependency is missing");
}

Status ModuleManager::check_dependencies(KernelModule &module) const
{
    const auto manifest = module.manifest();
    for (const auto &dependency : manifest.dependencies)
    {
        if (!dependency.required)
        {
            continue;
        }
        if (find(dependency.name) == nullptr)
        {
            return Status::not_found("module dependency is missing");
        }
    }
    return Status::success();
}

Status ModuleManager::validate_manifest(KernelModule &module) const
{
    const auto manifest = module.manifest();
    if (manifest.abi_version != kernel_module_abi_version)
    {
        return Status::unsupported("module ABI version is not supported");
    }
    if (manifest.resources.max_services != 0 && manifest.exported_services.size() > manifest.resources.max_services)
    {
        return Status::overflow("module service export budget exceeded");
    }
    const auto worker_budget = manifest.resources.max_threads == 0 ? static_cast<usize>(1) : manifest.resources.max_threads;
    if (manifest.threading == ModuleThreading::per_cpu && manifest.resources.max_threads != 0 &&
        kernel_process_scheduler_ != nullptr && kernel_process_scheduler_->cpu_count() > worker_budget)
    {
        return Status::overflow("module thread budget is below per-CPU worker count");
    }
    if (manifest.execution == ModuleExecution::kernel_process &&
        (manifest.capability_mask & module_capability_bit(ModuleCapability::owns_kernel_process)) == 0)
    {
        return Status::denied("kernel-process module is missing its process capability");
    }
    if (!manifest.exported_services.empty() &&
        (manifest.capability_mask & module_capability_bit(ModuleCapability::exports_services)) == 0)
    {
        return Status::denied("service-exporting module is missing its service capability");
    }
    if (!manifest.required_services.empty() &&
        (manifest.capability_mask & module_capability_bit(ModuleCapability::requires_services)) == 0)
    {
        return Status::denied("service-consuming module is missing its service capability");
    }
    if (manifest.threading == ModuleThreading::per_cpu &&
        (manifest.capability_mask & module_capability_bit(ModuleCapability::uses_per_cpu_workers)) == 0)
    {
        return Status::denied("per-CPU module is missing its worker capability");
    }
    return Status::success();
}

Status ModuleManager::visit(usize index)
{
    if (index >= modules_.size())
    {
        return Status::invalid_argument("module index out of range");
    }
    if (visit_state_[index] == VisitState::visited)
    {
        return Status::success();
    }
    if (visit_state_[index] == VisitState::visiting)
    {
        modules_[index]->fail("module dependency cycle");
        return Status::invalid_argument("module dependency cycle");
    }

    visit_state_[index] = VisitState::visiting;
    const auto manifest = modules_[index]->manifest();
    for (const auto &dependency : manifest.dependencies)
    {
        if (!dependency.required)
        {
            continue;
        }
        auto dependency_index = index_of(dependency.name);
        if (!dependency_index)
        {
            modules_[index]->fail("module dependency is missing");
            return dependency_index.status();
        }
        if (auto status = visit(dependency_index.value()); !status.ok())
        {
            return status;
        }
    }

    visit_state_[index] = VisitState::visited;
    return sorted_order_.push_back(index);
}

Status ModuleManager::sort_modules()
{
    sorted_order_ = {};
    for (usize i = 0; i < visit_state_.size(); ++i)
    {
        visit_state_[i] = VisitState::unvisited;
    }

    for (usize i = 0; i < modules_.size(); ++i)
    {
        if (auto status = check_dependencies(*modules_[i]); !status.ok())
        {
            modules_[i]->fail(status.message());
            return status;
        }
    }

    std::array<usize, max_kernel_modules> visit_order{};
    for (usize i = 0; i < modules_.size(); ++i)
    {
        visit_order[i] = i;
    }
    for (usize i = 0; i < modules_.size(); ++i)
    {
        for (usize j = i + 1; j < modules_.size(); ++j)
        {
            const auto left = modules_[visit_order[i]]->manifest();
            const auto right = modules_[visit_order[j]]->manifest();
            if (right.init_priority < left.init_priority)
            {
                const auto tmp = visit_order[i];
                visit_order[i] = visit_order[j];
                visit_order[j] = tmp;
            }
        }
    }

    for (usize i = 0; i < modules_.size(); ++i)
    {
        if (auto status = visit(visit_order[i]); !status.ok())
        {
            return status;
        }
    }

    return Status::success();
}

Status ModuleManager::check_required_services(KernelModule &module) const
{
    const auto manifest = module.manifest();
    for (const auto service_id : manifest.required_services)
    {
        if (!services_.contains(service_id))
        {
            module.fail("required service is missing");
            return Status::not_found("required service is missing");
        }
    }
    return Status::success();
}

Status ModuleManager::publish_services(KernelModule &module)
{
    const auto manifest = module.manifest();
    for (const auto service_id : manifest.exported_services)
    {
        auto *provider = module.service(service_id);
        if (provider == nullptr)
        {
            module.fail("module service provider is null");
            return Status::invalid_argument("module service provider is null");
        }
        if (services_.query_raw(service_id) == provider)
        {
            continue;
        }
        if (auto status = services_.register_service(service_id, provider); !status.ok())
        {
            module.fail(status.message());
            return status;
        }
    }
    return Status::success();
}

bool ModuleManager::started_order_contains(const KernelModule &module) const
{
    for (const auto *started : started_order_)
    {
        if (started == &module)
        {
            return true;
        }
    }
    return false;
}

Status ModuleManager::record_started(KernelModule &module)
{
    if (started_order_contains(module))
    {
        return Status::success();
    }
    return started_order_.push_back(&module);
}

Status ModuleManager::transition(KernelModule &module, ModuleState next, Status status)
{
    if (!status.ok())
    {
        module.fail(status.message());
        return status;
    }
    module.set_state(next);
    return Status::success();
}

Status ModuleManager::start_module(KernelModule &module)
{
    module.clear_failure();
    const auto manifest = module.manifest();
    if (auto status = validate_manifest(module); !status.ok())
    {
        module.fail(status.message());
        return status;
    }
    const bool already_recorded = started_order_contains(module);
    if (manifest.execution == ModuleExecution::kernel_process)
    {
        auto process = ensure_kernel_process(module);
        if (!process)
        {
            module.fail(process.status().message());
            return process.status();
        }
    }
    if (auto status = check_dependencies(module); !status.ok())
    {
        module.fail(status.message());
        return status;
    }
    if (auto status = transition(module, ModuleState::probed, module.probe()); !status.ok())
    {
        return status;
    }
    if (auto status = transition(module, ModuleState::initialized, module.init(services_)); !status.ok())
    {
        return status;
    }
    if (auto status = check_required_services(module); !status.ok())
    {
        return status;
    }
    if (auto status = transition(module, ModuleState::started, module.start(services_)); !status.ok())
    {
        return status;
    }
    if (auto status = publish_services(module); !status.ok())
    {
        return status;
    }
    if (auto status = record_started(module); !status.ok())
    {
        module.fail(status.message());
        return status;
    }
    if (manifest.execution == ModuleExecution::kernel_process && !already_recorded)
    {
        ++kernel_process_modules_;
    }
    return Status::success();
}

Status ModuleManager::start_all()
{
    if (auto status = sort_modules(); !status.ok())
    {
        return status;
    }

    started_order_ = {};
    kernel_process_modules_ = 0;
    for (const auto index : sorted_order_)
    {
        if (auto status = start_module(*modules_[index]); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status ModuleManager::restart_module(std::string_view name)
{
    auto *module = find(name);
    if (module == nullptr)
    {
        return Status::not_found("module is not registered");
    }
    if (module->manifest().restart_policy == ModuleRestartPolicy::never)
    {
        return Status::denied("module restart policy forbids restart");
    }
    if (module->state() == ModuleState::started)
    {
        if (auto status = module->stop(); !status.ok())
        {
            module->fail(status.message());
            return status;
        }
        module->set_state(ModuleState::stopped);
    }
    if (auto status = start_module(*module); !status.ok())
    {
        return status;
    }
    ++module->restart_count_;
    return Status::success();
}

Status ModuleManager::supervise_kernel_processes(StaticVector<ModuleProcessRestart, max_kernel_modules> &restarts)
{
    restarts.clear();
    if (kernel_process_scheduler_ == nullptr || kernel_process_arch_ == nullptr)
    {
        return Status::not_initialized("kernel module process is not bound to a scheduler");
    }

    for (usize i = 0; i < module_processes_.size(); ++i)
    {
        auto &record = module_processes_[i];
        if (record.pid != 0 && kernel_process_scheduler_->find(record.pid) != nullptr)
        {
            continue;
        }
        auto *module = find(record.module_name.view());
        if (module == nullptr || module->state() != ModuleState::started)
        {
            continue;
        }
        const auto manifest = module->manifest();
        if (manifest.restart_policy == ModuleRestartPolicy::never || manifest.restart_policy == ModuleRestartPolicy::manual)
        {
            continue;
        }

        const auto previous_pid = record.pid;
        if (auto status = restart_module(record.module_name.view()); !status.ok())
        {
            return status;
        }

        ModuleProcessRestart restart{.previous_pid = previous_pid, .pid = record.pid};
        if (auto status = assign_module_process_name(restart.process_name, record.module_name.view()); !status.ok())
        {
            return status;
        }
        if (auto status = restarts.push_back(restart); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status ModuleManager::stop_all()
{
    for (usize i = started_order_.size(); i != 0; --i)
    {
        auto &module = *started_order_[i - 1];
        if (auto status = transition(module, ModuleState::stopped, module.stop()); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status ModuleManager::shutdown_all()
{
    for (usize i = started_order_.size(); i != 0; --i)
    {
        auto &module = *started_order_[i - 1];
        if (auto status = module.shutdown(); !status.ok())
        {
            module.fail(status.message());
            return status;
        }
    }
    return Status::success();
}

std::string_view module_state_name(ModuleState state)
{
    switch (state)
    {
    case ModuleState::created:
        return "created";
    case ModuleState::probed:
        return "probed";
    case ModuleState::initialized:
        return "initialized";
    case ModuleState::started:
        return "started";
    case ModuleState::stopped:
        return "stopped";
    case ModuleState::failed:
        return "failed";
    }
    return "unknown";
}

std::string_view module_execution_name(ModuleExecution execution)
{
    switch (execution)
    {
    case ModuleExecution::inline_core:
        return "inline-core";
    case ModuleExecution::kernel_process:
        return "kernel-process";
    }
    return "unknown";
}

} // namespace ok
