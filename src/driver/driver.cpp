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

Status PciBusDriver::probe()
{
    return Status::success();
}

Status PciBusDriver::start()
{
    started_ = true;
    if (devices_.empty())
    {
        return add_emulated_device(PciDevice{
            .bus = 0,
            .slot = 20,
            .function = 0,
            .id = PciDeviceId{
                .vendor_id = 0x1b36,
                .device_id = 0x000d,
                .class_code = 0x0c,
                .subclass = 0x03,
                .programming_interface = 0x30,
            },
        });
    }
    return Status::success();
}

Status PciBusDriver::stop()
{
    started_ = false;
    return Status::success();
}

Status PciBusDriver::add_emulated_device(PciDevice device)
{
    if (!started_)
    {
        return Status::not_initialized("PCIe bus driver not started");
    }
    return devices_.push_back(device);
}

const PciDevice *PciBusDriver::find_class(u8 class_code, u8 subclass, u8 programming_interface) const
{
    for (const auto &device : devices_)
    {
        if (device.id.class_code == class_code && device.id.subclass == subclass &&
            device.id.programming_interface == programming_interface)
        {
            return &device;
        }
    }
    return nullptr;
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

Status UsbXhciControllerDriver::probe()
{
    return Status::success();
}

Status UsbXhciControllerDriver::start()
{
    started_ = true;
    if (devices_.empty())
    {
        if (auto status = attach_device(UsbDevice{
                .address = 1,
                .speed = UsbSpeed::full,
                .device_class = UsbDeviceClass::hid,
                .subclass = 1,
                .protocol = 1,
            });
            !status.ok())
        {
            return status;
        }
        return attach_device(UsbDevice{
            .address = 2,
            .speed = UsbSpeed::full,
            .device_class = UsbDeviceClass::hid,
            .subclass = 1,
            .protocol = 2,
        });
    }
    return Status::success();
}

Status UsbXhciControllerDriver::stop()
{
    started_ = false;
    return Status::success();
}

Status UsbXhciControllerDriver::attach_device(UsbDevice device)
{
    if (!started_)
    {
        return Status::not_initialized("xHCI controller not started");
    }
    return devices_.push_back(device);
}

const UsbDevice *UsbXhciControllerDriver::find_device(UsbDeviceClass device_class, u8 subclass, u8 protocol) const
{
    for (const auto &device : devices_)
    {
        if (device.device_class == device_class && device.subclass == subclass && device.protocol == protocol)
        {
            return &device;
        }
    }
    return nullptr;
}

Status UsbHidKeyboardDriver::probe()
{
    return Status::success();
}

Status UsbHidKeyboardDriver::start()
{
    started_ = true;
    return Status::success();
}

Status UsbHidKeyboardDriver::stop()
{
    started_ = false;
    return Status::success();
}

char UsbHidKeyboardDriver::translate_usage(u8 usage, bool shift) const
{
    if (usage >= 0x04 && usage <= 0x1d)
    {
        const char base = static_cast<char>('a' + (usage - 0x04));
        return shift ? static_cast<char>('A' + (usage - 0x04)) : base;
    }
    if (usage >= 0x1e && usage <= 0x26)
    {
        constexpr char normal[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        constexpr char shifted[] = {'!', '@', '#', '$', '%', '^', '&', '*', '('};
        return shift ? shifted[usage - 0x1e] : normal[usage - 0x1e];
    }
    if (usage == 0x27)
    {
        return shift ? ')' : '0';
    }
    if (usage == 0x28)
    {
        return '\n';
    }
    if (usage == 0x2c)
    {
        return ' ';
    }
    return 0;
}

Status UsbHidKeyboardDriver::feed_report(UsbKeyboardReport report)
{
    if (!started_)
    {
        return Status::not_initialized("USB HID keyboard not started");
    }
    const bool shift = (report.modifiers & 0x22u) != 0;
    for (const auto usage : report.keys)
    {
        if (usage == 0)
        {
            continue;
        }
        const auto ascii = translate_usage(usage, shift);
        if (ascii != 0)
        {
            if (auto status = events_.push(KeyEvent{.scancode = usage, .ascii = ascii, .pressed = true}); !status.ok())
            {
                return status;
            }
        }
    }
    return Status::success();
}

Result<KeyEvent> UsbHidKeyboardDriver::read_event()
{
    if (!started_)
    {
        return Status::not_initialized("USB HID keyboard not started");
    }
    return events_.pop();
}

Status UsbHidMouseDriver::probe()
{
    return Status::success();
}

Status UsbHidMouseDriver::start()
{
    started_ = true;
    return Status::success();
}

Status UsbHidMouseDriver::stop()
{
    started_ = false;
    return Status::success();
}

Status UsbHidMouseDriver::feed_report(UsbMouseReport report)
{
    if (!started_)
    {
        return Status::not_initialized("USB HID mouse not started");
    }
    return packets_.push(MousePacket{
        .delta_x = report.delta_x,
        .delta_y = report.delta_y,
        .left_button = (report.buttons & 0x01u) != 0,
        .right_button = (report.buttons & 0x02u) != 0,
        .middle_button = (report.buttons & 0x04u) != 0,
    });
}

Result<MousePacket> UsbHidMouseDriver::read_packet()
{
    if (!started_)
    {
        return Status::not_initialized("USB HID mouse not started");
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
