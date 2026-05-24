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
    return false;
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

Status DriverManager::bind_kernel_processes(sched::Scheduler &scheduler, arch::ArchOperations &arch, uptr entry_base,
                                            uptr stack_base)
{
    if (entry_base == 0 || stack_base == 0)
    {
        return Status::invalid_argument("driver kernel process context is invalid");
    }

    constexpr uptr entry_stride = 0x100;
    constexpr uptr stack_stride = 0x1000;
    for (usize i = 0; i < drivers_.size(); ++i)
    {
        auto *driver = drivers_[i];
        if (!should_register_driver_process(*driver))
        {
            continue;
        }
        bool already_bound = false;
        for (const auto &record : driver_processes_)
        {
            if (record.driver == driver && scheduler.find(record.pid) != nullptr)
            {
                already_bound = true;
                break;
            }
        }
        if (already_bound)
        {
            continue;
        }

        FixedString<sched::max_process_name> process_name;
        if (auto status = assign_kernel_process_name(process_name, "drv:", driver->name()); !status.ok())
        {
            return status;
        }

        auto process = scheduler.create_background_process(
            process_name.view(), arch.make_kernel_context(entry_base + static_cast<uptr>(i) * entry_stride,
                                                          stack_base + static_cast<uptr>(i) * stack_stride));
        if (!process)
        {
            return process.status();
        }
        if (auto status = driver_processes_.push_back(DriverProcessRecord{.driver = driver, .pid = process.value()});
            !status.ok())
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
