#include "ok/core/module.hpp"

namespace ok
{

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
    module.set_state(ModuleState::created);
    return modules_.push_back(&module);
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
        if (auto status = services_.register_service(service_id, &module); !status.ok())
        {
            module.fail(status.message());
            return status;
        }
    }
    return Status::success();
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

Status ModuleManager::start_all()
{
    if (auto status = sort_modules(); !status.ok())
    {
        return status;
    }

    started_order_ = {};
    for (const auto index : sorted_order_)
    {
        auto &module = *modules_[index];
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
        if (auto status = started_order_.push_back(&module); !status.ok())
        {
            module.fail(status.message());
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

} // namespace ok
