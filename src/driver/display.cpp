#include "ok/driver/driver.hpp"

extern "C" void ok_platform_display_write_line(const char *text, ok::usize size) __attribute__((weak));

namespace ok::driver
{

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
            const auto pixel =
                ((code >> ((x + y) & 7u)) & 1u) != 0 || value == '[' || value == ']' ? foreground : background;
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

Result<u32> FramebufferDisplayDriver::pixel_at(u32 x, u32 y) const
{
    if (!started_)
    {
        return Status::not_initialized("framebuffer driver not started");
    }
    if (x >= mode_.width || y >= mode_.height)
    {
        return Status::invalid_argument("pixel coordinate out of range");
    }
    return pixels_[static_cast<usize>(y) * mode_.width + x];
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

Status VirtioGpuPciDisplayDriver::probe()
{
    return Status::success();
}

Status VirtioGpuPciDisplayDriver::start()
{
    started_ = true;
    return Status::success();
}

Status VirtioGpuPciDisplayDriver::stop()
{
    started_ = false;
    bound_ = false;
    return Status::success();
}

Status VirtioGpuPciDisplayDriver::bind(const PciDevice &device)
{
    if (!started_)
    {
        return Status::not_initialized("virtio GPU driver not started");
    }
    if (device.id.vendor_id != 0x1af4 || device.id.class_code != 0x03)
    {
        return Status::invalid_argument("PCI device is not a virtio GPU display");
    }
    device_ = device;
    bound_ = true;
    return Status::success();
}

Status VirtioGpuPciDisplayDriver::present(const FramebufferDisplayDriver &framebuffer)
{
    if (!started_)
    {
        return Status::not_initialized("virtio GPU driver not started");
    }
    if (!bound_)
    {
        return Status::not_initialized("virtio GPU driver has no PCI device");
    }
    if (framebuffer.checksum() == 0)
    {
        return Status::invalid_argument("framebuffer is blank");
    }
    ++frames_presented_;
    return Status::success();
}

} // namespace ok::driver
