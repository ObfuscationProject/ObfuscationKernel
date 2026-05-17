#include "ok/driver/driver.hpp"

#include <algorithm>

namespace ok::driver {

Status DriverManager::start_all()
{
    for (auto& driver : drivers_) {
        if (auto status = driver->probe(); !status.ok()) {
            return status;
        }
        if (auto status = driver->start(); !status.ok()) {
            return status;
        }
    }
    return Status::success();
}

Driver* DriverManager::find(Class driver_class)
{
    const auto it = std::find_if(drivers_.begin(), drivers_.end(), [driver_class](const auto& driver) {
        return driver->driver_class() == driver_class;
    });
    return it == drivers_.end() ? nullptr : it->get();
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
    if (!started_) {
        return Status::not_initialized("console driver not started");
    }
    buffer_.append(text);
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

Status NullBlockDriver::probe()
{
    return Status::success();
}

Status NullBlockDriver::start()
{
    started_ = true;
    return Status::success();
}

Status NullBlockDriver::stop()
{
    started_ = false;
    return Status::success();
}

Status NullBlockDriver::read(uptr, std::span<std::byte> out)
{
    if (!started_) {
        return Status::not_initialized("null block driver not started");
    }
    std::ranges::fill(out, std::byte {0});
    return Status::success();
}

Status NullBlockDriver::write(uptr, std::span<const std::byte>)
{
    if (!started_) {
        return Status::not_initialized("null block driver not started");
    }
    return Status::success();
}

} // namespace ok::driver

