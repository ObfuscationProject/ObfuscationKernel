#include "ok/driver/driver.hpp"

namespace ok::driver
{
namespace
{

Status assign_kernel_process_name(FixedString<sched::max_process_name> &out, std::string_view prefix,
                                  std::string_view name)
{
    if (auto status = out.assign(prefix); !status.ok())
    {
        return status;
    }
    constexpr usize max_chars = sched::max_process_name - 1;
    const auto room = out.size() < max_chars ? max_chars - out.size() : 0;
    return out.append(name.substr(0, room));
}

bool should_register_driver_process(const Driver &driver)
{
    static_cast<void>(driver);
    return true;
}

} // namespace

Status DriverManager::add(Driver &driver)
{
    return drivers_.push_back(&driver);
}

Status DriverManager::start_all()
{
    for (auto *driver : drivers_)
    {
        if (auto status = driver->probe(); !status.ok())
        {
            return status;
        }
        if (auto status = driver->start(); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

DriverManager::DriverProcessRecord *DriverManager::find_process_record(Driver &driver)
{
    for (auto &record : driver_processes_)
    {
        if (record.driver == &driver)
        {
            return &record;
        }
    }
    return nullptr;
}

const DriverManager::DriverProcessRecord *DriverManager::find_process_record(const Driver &driver) const
{
    for (const auto &record : driver_processes_)
    {
        if (record.driver == &driver)
        {
            return &record;
        }
    }
    return nullptr;
}

Result<sched::ProcessId> DriverManager::ensure_kernel_process(Driver &driver, usize driver_index)
{
    if (!should_register_driver_process(driver))
    {
        return Status::invalid_argument("driver does not use a kernel process");
    }
    if (kernel_process_scheduler_ == nullptr || kernel_process_arch_ == nullptr)
    {
        return Status::not_initialized("driver kernel processes are not bound to a scheduler");
    }

    auto *existing_record = find_process_record(driver);
    if (existing_record != nullptr && kernel_process_scheduler_->find(existing_record->pid) != nullptr)
    {
        return existing_record->pid;
    }

    FixedString<sched::max_process_name> process_name;
    if (auto status = assign_kernel_process_name(process_name, "drv:", driver.name()); !status.ok())
    {
        return status;
    }

    constexpr uptr entry_stride = 0x100;
    constexpr uptr stack_stride = 0x1000;
    auto process = kernel_process_scheduler_->spawn(sched::ScheduleRequest{
        .name = process_name.view(),
        .initial_context = kernel_process_arch_->make_kernel_context(
            kernel_process_entry_base_ + static_cast<uptr>(driver_index) * entry_stride,
            kernel_process_stack_base_ + static_cast<uptr>(driver_index) * stack_stride),
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

    if (existing_record != nullptr)
    {
        existing_record->pid = process.value();
        return process.value();
    }
    if (auto status = driver_processes_.push_back(DriverProcessRecord{.driver = &driver, .pid = process.value()});
        !status.ok())
    {
        return status;
    }
    return process.value();
}

Status DriverManager::bind_kernel_processes(sched::Scheduler &scheduler, arch::ArchOperations &arch, uptr entry_base,
                                            uptr stack_base)
{
    if (entry_base == 0 || stack_base == 0)
    {
        return Status::invalid_argument("driver kernel process context is invalid");
    }

    kernel_process_scheduler_ = &scheduler;
    kernel_process_arch_ = &arch;
    kernel_process_entry_base_ = entry_base;
    kernel_process_stack_base_ = stack_base;
    for (usize i = 0; i < drivers_.size(); ++i)
    {
        auto *driver = drivers_[i];
        if (!should_register_driver_process(*driver))
        {
            continue;
        }
        if (auto process = ensure_kernel_process(*driver, i); !process)
        {
            return process.status();
        }
    }
    return Status::success();
}

Status DriverManager::supervise_kernel_processes(StaticVector<DriverProcessRestart, max_drivers> &restarts)
{
    restarts.clear();
    if (kernel_process_scheduler_ == nullptr || kernel_process_arch_ == nullptr)
    {
        return Status::not_initialized("driver kernel processes are not bound to a scheduler");
    }

    for (usize i = 0; i < drivers_.size(); ++i)
    {
        auto *driver = drivers_[i];
        if (!should_register_driver_process(*driver))
        {
            continue;
        }
        auto *record = find_process_record(*driver);
        const auto previous_pid = record == nullptr ? sched::ProcessId{0} : record->pid;
        if (record != nullptr && kernel_process_scheduler_->find(record->pid) != nullptr)
        {
            continue;
        }
        auto process = ensure_kernel_process(*driver, i);
        if (!process)
        {
            return process.status();
        }
        DriverProcessRestart restart{.previous_pid = previous_pid, .pid = process.value()};
        if (auto status = assign_kernel_process_name(restart.process_name, "drv:", driver->name()); !status.ok())
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

Driver *DriverManager::find(Class driver_class)
{
    for (auto *driver : drivers_)
    {
        if (driver->driver_class() == driver_class)
        {
            return driver;
        }
    }
    return nullptr;
}

Result<sched::ProcessId> DriverManager::kernel_process_id(std::string_view driver_name) const
{
    for (const auto *driver : drivers_)
    {
        if (driver->name() != driver_name)
        {
            continue;
        }
        const auto *record = find_process_record(*driver);
        if (record == nullptr)
        {
            return Status::not_found("driver kernel process not found");
        }
        return record->pid;
    }
    return Status::not_found("driver not found");
}

Status ConsoleDriver::probe()
{
    return Status::success();
}

Status ConsoleDriver::start()
{
    started_ = true;
    return Status::success();
}

Status ConsoleDriver::stop()
{
    started_ = false;
    return Status::success();
}

Status ConsoleDriver::write(std::string_view text)
{
    if (!started_)
    {
        return Status::not_initialized("console driver not started");
    }
    if (buffer_size_ + text.size() > buffer_.size())
    {
        return Status::overflow("console buffer full");
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        buffer_[buffer_size_++] = text[i];
    }
    return Status::success();
}

Status TimerDriver::probe()
{
    return Status::success();
}

Status TimerDriver::start()
{
    started_ = true;
    return Status::success();
}

Status TimerDriver::stop()
{
    started_ = false;
    return Status::success();
}

} // namespace ok::driver
