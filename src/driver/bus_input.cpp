#include "ok/driver/driver.hpp"

namespace ok::driver
{

Status PciBusDriver::probe()
{
    return Status::success();
}

Status PciBusDriver::start()
{
    started_ = true;
    if (devices_.empty())
    {
        if (auto status = add_emulated_device(PciDevice{
                .bus = 0,
                .slot = 20,
                .function = 0,
                .id =
                    PciDeviceId{
                        .vendor_id = 0x1b36,
                        .device_id = 0x000d,
                        .class_code = 0x0c,
                        .subclass = 0x03,
                        .programming_interface = 0x30,
                    },
            });
            !status.ok())
        {
            return status;
        }
        if (auto status = add_emulated_device(PciDevice{
                .bus = 0,
                .slot = 3,
                .function = 0,
                .id =
                    PciDeviceId{
                        .vendor_id = 0x1af4,
                        .device_id = 0x1042,
                        .class_code = 0x01,
                        .subclass = 0x00,
                        .programming_interface = 0x00,
                    },
            });
            !status.ok())
        {
            return status;
        }
        return add_emulated_device(PciDevice{
            .bus = 0,
            .slot = 2,
            .function = 0,
            .id =
                PciDeviceId{
                    .vendor_id = 0x1af4,
                    .device_id = 0x1050,
                    .class_code = 0x03,
                    .subclass = 0x00,
                    .programming_interface = 0x00,
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
        0,    0,   '1', '2',  '3', '4', '5', '6', '7',  '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
        't',  'y', 'u', 'i',  'o', 'p', '[', ']', '\n', 0,   'a', 's', 'd', 'f', 'g',  'h',  'j', 'k', 'l', ';',
        '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', 'b',  'n', 'm', ',', '.', '/', 0,    '*',  0,   ' ',
    };
    constexpr char shifted[] = {
        0,   0,   '!', '@', '#', '$', '%', '^', '&',  '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
        'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S', 'D', 'F', 'G',  'H',  'J', 'K', 'L', ':',
        '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B',  'N', 'M', '<', '>', '?', 0,    '*',  0,   ' ',
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

} // namespace ok::driver
