#include "ok/driver/driver.hpp"

extern "C" void ok_platform_display_write_line(const char *text, ok::usize size) __attribute__((weak));

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

Status KeyboardDriver::probe()
{
    return Status::success();
}

Status KeyboardDriver::start()
{
    started_ = true;
    return Status::success();
}

Status KeyboardDriver::stop()
{
    started_ = false;
    return Status::success();
}

char KeyboardDriver::translate(u8 scancode) const
{
    constexpr char normal[] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8',
        '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
        't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
        '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
        'm', ',', '.', '/', 0, '*', 0, ' ',
    };
    constexpr char shifted[] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*',
        '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
        'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
        '"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N',
        'M', '<', '>', '?', 0, '*', 0, ' ',
    };
    if (scancode >= sizeof(normal))
    {
        return 0;
    }
    return (left_shift_ || right_shift_) ? shifted[scancode] : normal[scancode];
}

Status KeyboardDriver::feed_scancode(u8 scancode)
{
    if (!started_)
    {
        return Status::not_initialized("keyboard driver not started");
    }
    const bool released = (scancode & 0x80u) != 0;
    const auto key = static_cast<u8>(scancode & 0x7fu);
    if (key == 0x2a)
    {
        left_shift_ = !released;
        return Status::success();
    }
    if (key == 0x36)
    {
        right_shift_ = !released;
        return Status::success();
    }

    const char ascii = released ? 0 : translate(key);
    return events_.push(KeyEvent{.scancode = key, .ascii = ascii, .pressed = !released});
}

Result<KeyEvent> KeyboardDriver::read_event()
{
    if (!started_)
    {
        return Status::not_initialized("keyboard driver not started");
    }
    return events_.pop();
}

Status Ps2MouseDriver::probe()
{
    return Status::success();
}

Status Ps2MouseDriver::start()
{
    started_ = true;
    return Status::success();
}

Status Ps2MouseDriver::stop()
{
    started_ = false;
    return Status::success();
}

Status Ps2MouseDriver::feed_packet(MousePacket packet)
{
    if (!started_)
    {
        return Status::not_initialized("mouse driver not started");
    }
    return packets_.push(packet);
}

Result<MousePacket> Ps2MouseDriver::read_packet()
{
    if (!started_)
    {
        return Status::not_initialized("mouse driver not started");
    }
    return packets_.pop();
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
    text_size_ = 0;
    cursor_row_ = 0;
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

void FramebufferDisplayDriver::draw_cell(u32 column, u32 row, char value)
{
    constexpr u32 cell_width = 2;
    constexpr u32 cell_height = 4;
    constexpr u32 foreground = 0xffd8f3ffu;
    constexpr u32 background = 0xff061018u;
    const auto origin_x = column * cell_width;
    const auto origin_y = row * cell_height;
    const auto code = static_cast<u8>(value);
    for (u32 y = 0; y < cell_height; ++y)
    {
        for (u32 x = 0; x < cell_width; ++x)
        {
            const auto pixel = ((code >> ((x + y) & 7u)) & 1u) != 0 || value == '[' || value == ']'
                                   ? foreground
                                   : background;
            pixels_[static_cast<usize>(origin_y + y) * mode_.width + origin_x + x] = pixel;
        }
    }
}

Status FramebufferDisplayDriver::write_line(std::string_view text)
{
    if (!started_)
    {
        return Status::not_initialized("framebuffer driver not started");
    }
    if (cursor_row_ >= display_text_rows)
    {
        return Status::overflow("display text rows exhausted");
    }

    const auto row = cursor_row_++;
    const auto count = text.size() < display_text_columns ? text.size() : display_text_columns;
    if (text_size_ + count + 1 > text_.size())
    {
        return Status::overflow("display text buffer full");
    }

    for (usize i = 0; i < count; ++i)
    {
        const auto value = text[i];
        text_[text_size_++] = value;
        draw_cell(static_cast<u32>(i), row, value);
    }
    text_[text_size_++] = '\n';
    if (ok_platform_display_write_line != nullptr)
    {
        ok_platform_display_write_line(text.data(), count);
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
