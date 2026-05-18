#include "ok/driver/driver.hpp"

namespace ok::driver
{

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
    if (!started_)
    {
        return Status::not_initialized("null block driver not started");
    }
    for (auto &byte : out)
    {
        byte = std::byte{0};
    }
    return Status::success();
}

Status NullBlockDriver::write(uptr, std::span<const std::byte>)
{
    if (!started_)
    {
        return Status::not_initialized("null block driver not started");
    }
    return Status::success();
}

Status FramebufferDisplayDriver::probe()
{
    return Status::success();
}

Status FramebufferDisplayDriver::start()
{
    started_ = true;
    return clear(0xff000000u);
}

Status FramebufferDisplayDriver::stop()
{
    started_ = false;
    return Status::success();
}

Status FramebufferDisplayDriver::clear(u32 rgba)
{
    if (!started_)
    {
        return Status::not_initialized("framebuffer driver not started");
    }
    for (auto &pixel : pixels_)
    {
        pixel = rgba;
    }
    return Status::success();
}

Status FramebufferDisplayDriver::put_pixel(u32 x, u32 y, u32 rgba)
{
    if (!started_)
    {
        return Status::not_initialized("framebuffer driver not started");
    }
    if (x >= mode_.width || y >= mode_.height)
    {
        return Status::invalid_argument("pixel coordinate out of range");
    }
    pixels_[static_cast<usize>(y) * mode_.width + x] = rgba;
    return Status::success();
}

Status FramebufferDisplayDriver::fill_rect(u32 x, u32 y, u32 width, u32 height, u32 rgba)
{
    if (!started_)
    {
        return Status::not_initialized("framebuffer driver not started");
    }
    if (x + width > mode_.width || y + height > mode_.height)
    {
        return Status::invalid_argument("rectangle out of range");
    }
    for (u32 row = 0; row < height; ++row)
    {
        for (u32 column = 0; column < width; ++column)
        {
            pixels_[static_cast<usize>(y + row) * mode_.width + x + column] = rgba;
        }
    }
    return Status::success();
}

u64 FramebufferDisplayDriver::checksum() const
{
    u64 value = 1469598103934665603ull;
    for (const auto pixel : pixels_)
    {
        value ^= pixel;
        value *= 1099511628211ull;
    }
    return value;
}

} // namespace ok::driver
