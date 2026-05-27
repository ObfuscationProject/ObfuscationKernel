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

bool resource_overlaps(const OkResource &left, const OkResource &right)
{
    if (left.kind != right.kind)
    {
        return false;
    }
    if (left.length == 0 || right.length == 0)
    {
        return left.base == right.base;
    }
    const auto left_end = left.base + left.length;
    const auto right_end = right.base + right.length;
    return left.base < right_end && right.base < left_end;
}

} // namespace

Status OkResourceManager::add(OkResource resource)
{
    if (resource.length == 0 && resource.kind != OkResourceKind::irq)
    {
        return Status::invalid_argument("resource length is zero");
    }
    for (const auto &existing : resources_)
    {
        if (resource_overlaps(existing, resource))
        {
            return Status::already_exists("resource overlaps an existing resource");
        }
    }
    return resources_.push_back(resource);
}

Result<OkResource *> OkResourceManager::claim(OkResourceKind kind, uptr base, usize length)
{
    for (auto &resource : resources_)
    {
        if (resource.kind != kind || resource.base != base)
        {
            continue;
        }
        if (length != 0 && resource.length < length)
        {
            return Status::invalid_argument("resource claim length exceeds resource");
        }
        if (resource.claimed)
        {
            return Status::busy("resource is already claimed");
        }
        resource.claimed = true;
        return &resource;
    }
    return Status::not_found("resource not found");
}

Status OkResourceManager::release(OkResourceKind kind, uptr base)
{
    for (auto &resource : resources_)
    {
        if (resource.kind != kind || resource.base != base)
        {
            continue;
        }
        if (!resource.claimed)
        {
            return Status::invalid_argument("resource is not claimed");
        }
        resource.claimed = false;
        return Status::success();
    }
    return Status::not_found("resource not found");
}

usize OkResourceManager::claimed_count() const
{
    usize count = 0;
    for (const auto &resource : resources_)
    {
        if (resource.claimed)
        {
            ++count;
        }
    }
    return count;
}

const OkResource *OkResourceManager::find(OkResourceKind kind, uptr base) const
{
    for (const auto &resource : resources_)
    {
        if (resource.kind == kind && resource.base == base)
        {
            return &resource;
        }
    }
    return nullptr;
}

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
    if (driver_manifest.device_class != OkDeviceClass::generic &&
        driver_manifest.device_class != ok_device_class_for(device))
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
    if (resources_.resource_count() == 0)
    {
        static_cast<void>(resources_.add(OkResource{.kind = OkResourceKind::mmio, .base = 0x1000, .length = 0x1000}));
        static_cast<void>(resources_.add(OkResource{.kind = OkResourceKind::irq, .base = irq_.vector, .length = 1}));
    }
    bound_count_ = 0;
    for (auto &device : devices_)
    {
        if (device.device_class == OkDeviceClass::generic)
        {
            device.device_class = ok_device_class_for(device);
        }
        for (auto *driver : drivers_)
        {
            if (!driver->match(device))
            {
                continue;
            }
            OkProbeContext context{.device = &device, .mmio = &mmio_, .irq = &irq_, .resources = &resources_};
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
            OkProbeContext context{.device = &device, .mmio = &mmio_, .irq = &irq_, .resources = &resources_};
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

OkDeviceClass ok_device_class_for(const OkDevice &device)
{
    if (device.device_class != OkDeviceClass::generic)
    {
        return device.device_class;
    }
    const auto klass = (device.id.class_code >> 16u) & 0xffu;
    switch (klass)
    {
    case 0x01:
        return OkDeviceClass::block;
    case 0x02:
        return OkDeviceClass::network;
    case 0x03:
        return OkDeviceClass::display;
    case 0x09:
    case 0x0c:
        return OkDeviceClass::input;
    default:
        return OkDeviceClass::generic;
    }
}

std::string_view ok_device_class_name(OkDeviceClass device_class)
{
    switch (device_class)
    {
    case OkDeviceClass::generic:
        return "generic";
    case OkDeviceClass::block:
        return "block";
    case OkDeviceClass::network:
        return "network";
    case OkDeviceClass::input:
        return "input";
    case OkDeviceClass::display:
        return "display";
    }
    return "unknown";
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
