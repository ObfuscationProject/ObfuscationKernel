#include "ok/driver/abi.hpp"

namespace ok::driver
{
namespace
{

bool id_matches(const OkDeviceId &driver_id, const OkDeviceId &device_id)
{
    const bool vendor = driver_id.vendor == 0xffff'ffffu || driver_id.vendor == device_id.vendor;
    const bool device = driver_id.device == 0xffff'ffffu || driver_id.device == device_id.device;
    const bool klass = driver_id.class_code == 0xffff'ffffu || driver_id.class_code == device_id.class_code;
    return vendor && device && klass;
}

} // namespace

u32 OkMmioRegion::readl(usize index) const
{
    if (!mapped || index >= words.size())
    {
        return 0;
    }
    return words[index];
}

Status OkMmioRegion::writel(usize index, u32 value)
{
    if (!mapped)
    {
        return Status::not_initialized("MMIO region is not mapped");
    }
    if (index >= words.size())
    {
        return Status::invalid_argument("MMIO offset out of range");
    }
    words[index] = value;
    return Status::success();
}

Status OkMutex::lock()
{
    if (locked_)
    {
        return Status::would_block("mutex is already locked");
    }
    locked_ = true;
    return Status::success();
}

Status OkMutex::unlock()
{
    if (!locked_)
    {
        return Status::invalid_argument("mutex is not locked");
    }
    locked_ = false;
    return Status::success();
}

bool OkDriverModule::match(const OkDevice &device) const
{
    const auto driver_manifest = manifest();
    if (driver_manifest.bus != device.bus)
    {
        return false;
    }
    for (const auto &id : driver_manifest.ids)
    {
        if (id_matches(id, device.id))
        {
            return true;
        }
    }
    return false;
}

Status OkDriverModule::attach(OkProbeContext &context)
{
    if (context.device == nullptr)
    {
        return Status::invalid_argument("driver attach context has no device");
    }
    context.device->attached = true;
    return Status::success();
}

Status OkDriverModule::start()
{
    return Status::success();
}

Status OkDriverModule::suspend()
{
    return Status::success();
}

Status OkDriverModule::resume()
{
    return Status::success();
}

Status OkDriverModule::remove(OkProbeContext &context)
{
    if (context.device != nullptr)
    {
        context.device->attached = false;
    }
    return Status::success();
}

Status OkDriverModule::shutdown()
{
    return Status::success();
}

Status OkDriverRegistry::register_device(OkDevice device)
{
    return devices_.push_back(device);
}

Status OkDriverRegistry::register_driver(OkDriverModule &driver)
{
    return drivers_.push_back(&driver);
}

Status OkDriverRegistry::bind_all()
{
    mmio_.mapped = true;
    irq_ = OkIrqHandle{.vector = 32, .registered = true};
    bound_count_ = 0;
    for (auto &device : devices_)
    {
        for (auto *driver : drivers_)
        {
            if (!driver->match(device))
            {
                continue;
            }
            OkProbeContext context{.device = &device, .mmio = &mmio_, .irq = &irq_};
            if (auto status = driver->probe(context); !status.ok())
            {
                return status;
            }
            if (auto status = driver->attach(context); !status.ok())
            {
                return status;
            }
            if (auto status = driver->start(); !status.ok())
            {
                return status;
            }
            ++bound_count_;
            break;
        }
    }
    return Status::success();
}

Status OkDriverRegistry::remove_all()
{
    for (auto &device : devices_)
    {
        if (!device.attached)
        {
            continue;
        }
        for (auto *driver : drivers_)
        {
            if (!driver->match(device))
            {
                continue;
            }
            OkProbeContext context{.device = &device, .mmio = &mmio_, .irq = &irq_};
            if (auto status = driver->remove(context); !status.ok())
            {
                return status;
            }
            if (bound_count_ > 0)
            {
                --bound_count_;
            }
            break;
        }
    }
    return Status::success();
}

namespace linux_compat
{

Status LinuxPciShim::register_driver(pci_driver &driver)
{
    if (driver.probe == nullptr || driver.remove == nullptr)
    {
        return Status::invalid_argument("Linux PCI shim driver callbacks are incomplete");
    }
    driver_ = &driver;
    return Status::success();
}

Status LinuxPciShim::bind(OkDevice &device)
{
    if (driver_ == nullptr)
    {
        return Status::not_initialized("Linux PCI shim has no registered driver");
    }
    for (const auto &id : driver_->ids)
    {
        if ((id.vendor == 0xffff'ffffu || id.vendor == device.id.vendor) &&
            (id.device == 0xffff'ffffu || id.device == device.id.device))
        {
            if (auto status = driver_->probe(device, id); !status.ok())
            {
                return status;
            }
            device.attached = true;
            ++probe_count_;
            return Status::success();
        }
    }
    return Status::not_found("Linux PCI shim driver did not match device");
}

Status LinuxPciShim::remove(OkDevice &device)
{
    if (driver_ == nullptr)
    {
        return Status::not_initialized("Linux PCI shim has no registered driver");
    }
    if (auto status = driver_->remove(device); !status.ok())
    {
        return status;
    }
    device.attached = false;
    ++remove_count_;
    return Status::success();
}

OkMmioRegion *LinuxPciShim::ioremap(uptr, usize)
{
    mmio_.mapped = true;
    return &mmio_;
}

Status LinuxPciShim::iounmap(OkMmioRegion *region)
{
    if (region == nullptr || region != &mmio_)
    {
        return Status::invalid_argument("invalid MMIO unmap");
    }
    region->mapped = false;
    return Status::success();
}

void *LinuxPciShim::kmalloc(usize size)
{
    if (heap_used_ || size > heap_.size())
    {
        return nullptr;
    }
    heap_used_ = true;
    return heap_.data();
}

Status LinuxPciShim::kfree(void *pointer)
{
    if (pointer != heap_.data() || !heap_used_)
    {
        return Status::invalid_argument("invalid shim allocation free");
    }
    heap_used_ = false;
    return Status::success();
}

Status LinuxPciShim::request_irq(OkIrqHandle &handle, u32 vector)
{
    if (handle.registered)
    {
        return Status::already_exists("IRQ already registered");
    }
    handle = OkIrqHandle{.vector = vector, .registered = true};
    return Status::success();
}

Status LinuxPciShim::free_irq(OkIrqHandle &handle)
{
    if (!handle.registered)
    {
        return Status::invalid_argument("IRQ is not registered");
    }
    handle.registered = false;
    return Status::success();
}

} // namespace linux_compat

OkDriverManifest NativeFakePciDriver::manifest() const
{
    static const std::array ids{
        OkDeviceId{.vendor = 0x1af4, .device = 0x1000, .class_code = 0x02},
    };
    return OkDriverManifest{
        .name = "native-fake-pci",
        .version = "1",
        .bus = OkBusType::pci,
        .ids = ids,
    };
}

Status NativeFakePciDriver::probe(OkProbeContext &context)
{
    if (context.device == nullptr || context.mmio == nullptr || context.irq == nullptr)
    {
        return Status::invalid_argument("native fake PCI driver probe context is incomplete");
    }
    if (auto status = context.mmio->writel(0, 0x0badc0de); !status.ok())
    {
        return status;
    }
    if (!context.irq->registered)
    {
        return Status::not_initialized("native fake PCI driver IRQ is not registered");
    }
    probed_ = true;
    removed_ = false;
    return Status::success();
}

Status NativeFakePciDriver::remove(OkProbeContext &context)
{
    removed_ = true;
    if (context.device != nullptr)
    {
        context.device->attached = false;
    }
    return Status::success();
}

} // namespace ok::driver
