#include "ok/driver/driver.hpp"

namespace ok::driver {

Status DriverManager::add(Driver& driver)
{
    return drivers_.push_back(&driver);
}

Status DriverManager::start_all()
{
    for (auto* driver : drivers_) {
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
    for (auto* driver : drivers_) {
        if (driver->driver_class() == driver_class) {
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
    if (!started_) {
        return Status::not_initialized("console driver not started");
    }
    if (buffer_size_ + text.size() > buffer_.size()) {
        return Status::overflow("console buffer full");
    }
    for (usize i = 0; i < text.size(); ++i) {
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
    for (auto& byte : out) {
        byte = std::byte {0};
    }
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
