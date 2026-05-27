#include "roadmap_tests.hpp"

#include "ok/driver/abi.hpp"

#include <array>

namespace ok
{
namespace
{

class LifecycleFakePciDriver final : public driver::OkDriverModule
{
  public:
    [[nodiscard]] driver::OkDriverManifest manifest() const override
    {
        static constexpr std::array ids{
            driver::OkDeviceId{.vendor = 0x1af4, .device = 0x1000, .class_code = 0x02},
        };
        return driver::OkDriverManifest{
            .name = "lifecycle-fake-pci",
            .version = "1",
            .bus = driver::OkBusType::pci,
            .ids = ids,
        };
    }

    Status probe(driver::OkProbeContext &context) override
    {
        if (context.device == nullptr || context.mmio == nullptr || context.irq == nullptr)
        {
            return Status::invalid_argument("lifecycle fake probe context is incomplete");
        }
        ++probes;
        return Status::success();
    }

    Status attach(driver::OkProbeContext &context) override
    {
        if (auto status = OkDriverModule::attach(context); !status.ok())
        {
            return status;
        }
        ++attaches;
        return Status::success();
    }

    Status start() override
    {
        ++starts;
        return Status::success();
    }

    Status suspend() override
    {
        ++suspends;
        return Status::success();
    }

    Status resume() override
    {
        ++resumes;
        return Status::success();
    }

    Status remove(driver::OkProbeContext &context) override
    {
        if (auto status = OkDriverModule::remove(context); !status.ok())
        {
            return status;
        }
        ++removes;
        return Status::success();
    }

    Status shutdown() override
    {
        ++shutdowns;
        return Status::success();
    }

    usize probes{0};
    usize attaches{0};
    usize starts{0};
    usize suspends{0};
    usize resumes{0};
    usize removes{0};
    usize shutdowns{0};
};

Status test_driver_abi_helpers()
{
    driver::OkDmaBuffer dma{};
    dma.size = 32;
    dma.bytes[0] = std::byte{0xa5};
    driver::OkIoPortRegion ports{.base = 0x3f8, .length = 8};
    driver::OkWorkQueue work{.queued = 1};
    driver::OkTimer timer{.deadline_ticks = 20, .armed = true};
    if (dma.size != 32 || dma.bytes[0] != std::byte{0xa5} || ports.length != 8 || work.queued != 1 || !timer.armed)
    {
        return Status::fault("driver ABI value helpers failed");
    }

    driver::OkRefCount refcount;
    refcount.get();
    if (refcount.value() != 2 || refcount.put() || !refcount.put())
    {
        return Status::fault("driver ABI refcount validation failed");
    }

    driver::OkMutex mutex;
    if (!mutex.lock().ok() || mutex.lock().code() != StatusCode::would_block || !mutex.unlock().ok() ||
        mutex.unlock().code() != StatusCode::invalid_argument)
    {
        return Status::fault("driver ABI mutex validation failed");
    }

    driver::OkMmioRegion mmio{};
    if (mmio.readl(0) != 0 || mmio.writel(0, 1).code() != StatusCode::not_initialized)
    {
        return Status::fault("driver ABI unmapped MMIO validation failed");
    }
    mmio.mapped = true;
    if (!mmio.writel(0, 0x1234).ok() || mmio.readl(0) != 0x1234 ||
        mmio.writel(driver::ok_mmio_words, 0).code() != StatusCode::invalid_argument)
    {
        return Status::fault("driver ABI mapped MMIO validation failed");
    }

    return Status::success();
}

Status test_driver_resource_manager_and_classes()
{
    driver::OkResourceManager resources;
    if (auto status =
            resources.add(driver::OkResource{.kind = driver::OkResourceKind::mmio, .base = 0x1000, .length = 0x100});
        !status.ok())
    {
        return status;
    }
    if (resources.add(driver::OkResource{.kind = driver::OkResourceKind::mmio, .base = 0x1080, .length = 0x40})
            .code() != StatusCode::already_exists)
    {
        return Status::fault("driver resource manager accepted overlapping MMIO resources");
    }
    auto claimed = resources.claim(driver::OkResourceKind::mmio, 0x1000, 0x80);
    if (!claimed || !claimed.value()->claimed || resources.claimed_count() != 1 ||
        resources.claim(driver::OkResourceKind::mmio, 0x1000, 0x80).status().code() != StatusCode::busy ||
        !resources.release(driver::OkResourceKind::mmio, 0x1000).ok() || resources.claimed_count() != 0)
    {
        return Status::fault("driver resource claim/release validation failed");
    }

    driver::OkDevice network_device{
        .bus = driver::OkBusType::pci,
        .id = driver::OkDeviceId{.vendor = 0x1af4, .device = 0x1000, .class_code = 0x020000},
    };
    if (driver::ok_device_class_for(network_device) != driver::OkDeviceClass::network ||
        driver::ok_device_class_name(driver::OkDeviceClass::network) != "network")
    {
        return Status::fault("driver device class inference failed");
    }

    struct NetworkOnlyDriver final : public driver::OkDriverModule
    {
        [[nodiscard]] driver::OkDriverManifest manifest() const override
        {
            static constexpr std::array ids{
                driver::OkDeviceId{.vendor = 0x1af4, .device = 0x1000, .class_code = 0xffff'ffffu},
            };
            return driver::OkDriverManifest{
                .name = "network-only",
                .version = "1",
                .bus = driver::OkBusType::pci,
                .ids = ids,
                .device_class = driver::OkDeviceClass::network,
            };
        }

        Status probe(driver::OkProbeContext &context) override
        {
            if (context.resources == nullptr || context.device == nullptr)
            {
                return Status::invalid_argument("network-only driver context is incomplete");
            }
            ++probes;
            return Status::success();
        }

        usize probes{0};
    } network_driver;

    driver::OkDriverRegistry registry;
    if (auto status = registry.register_device(network_device); !status.ok())
    {
        return status;
    }
    if (auto status = registry.register_driver(network_driver); !status.ok())
    {
        return status;
    }
    if (auto status = registry.bind_all(); !status.ok())
    {
        return status;
    }
    if (network_driver.probes != 1 || registry.bound_count() != 1 || registry.resources().resource_count() == 0)
    {
        return Status::fault("Linux-style driver class/resource binding failed");
    }
    return Status::success();
}

Status test_native_driver_lifecycle()
{
    LifecycleFakePciDriver lifecycle;
    driver::OkDevice device{
        .bus = driver::OkBusType::pci,
        .id = driver::OkDeviceId{.vendor = 0x1af4, .device = 0x1000, .class_code = 0x02},
    };
    driver::OkMmioRegion mmio{};
    mmio.mapped = true;
    driver::OkIrqHandle irq{.vector = 51, .registered = true};
    driver::OkProbeContext context{.device = &device, .mmio = &mmio, .irq = &irq};
    if (!lifecycle.match(device))
    {
        return Status::fault("native driver match validation failed");
    }

    const auto manifest = lifecycle.manifest();
    driver::OkDriverOps ops{
        .match =
            [](const driver::OkDevice &candidate, const driver::OkDriverManifest &driver_manifest) {
                return candidate.bus == driver_manifest.bus ? Status::success()
                                                            : Status::not_found("driver did not match");
            },
        .probe =
            [](driver::OkProbeContext &probe_context) {
                return probe_context.device == nullptr ? Status::invalid_argument("missing probe device")
                                                       : Status::success();
            },
        .remove =
            [](driver::OkProbeContext &remove_context) {
                return remove_context.device == nullptr ? Status::invalid_argument("missing remove device")
                                                        : Status::success();
            },
    };
    if (!ops.match(device, manifest).ok() || !ops.probe(context).ok() || !ops.remove(context).ok())
    {
        return Status::fault("native driver ops callback validation failed");
    }

    if (!lifecycle.probe(context).ok() || !lifecycle.attach(context).ok() || !device.attached ||
        !lifecycle.start().ok() || !lifecycle.suspend().ok() || !lifecycle.resume().ok() ||
        !lifecycle.remove(context).ok() || device.attached || !lifecycle.shutdown().ok())
    {
        return Status::fault("native driver lifecycle validation failed");
    }
    if (lifecycle.probes != 1 || lifecycle.attaches != 1 || lifecycle.starts != 1 || lifecycle.suspends != 1 ||
        lifecycle.resumes != 1 || lifecycle.removes != 1 || lifecycle.shutdowns != 1)
    {
        return Status::fault("native driver lifecycle counters failed");
    }

    driver::OkDriverRegistry registry;
    driver::NativeFakePciDriver native;
    if (auto status = registry.register_device(driver::OkDevice{
            .bus = driver::OkBusType::platform,
            .id = driver::OkDeviceId{.vendor = 0x1111, .device = 0x2222, .class_code = 0xff},
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = registry.register_device(driver::OkDevice{
            .bus = driver::OkBusType::pci,
            .id = driver::OkDeviceId{.vendor = 0x1af4, .device = 0x1000, .class_code = 0x02},
            .resources = {0x1000, 0, 0, 0, 0, 0},
        });
        !status.ok())
    {
        return status;
    }
    if (auto status = registry.register_driver(native); !status.ok())
    {
        return status;
    }
    if (auto status = registry.bind_all(); !status.ok())
    {
        return status;
    }
    if (!native.probed() || registry.device_count() != 2 || registry.driver_count() != 1 ||
        registry.bound_count() != 1 || registry.mmio().readl(0) != 0x0badc0de || !registry.irq().registered)
    {
        return Status::fault("native driver ABI validation failed");
    }
    if (auto status = registry.remove_all(); !status.ok())
    {
        return status;
    }
    if (!native.removed() || registry.bound_count() != 0)
    {
        return Status::fault("native driver remove validation failed");
    }

    return Status::success();
}

Status test_linux_driver_shim()
{
    driver::linux_compat::LinuxPciShim shim;
    driver::OkDevice device{
        .bus = driver::OkBusType::pci,
        .id = driver::OkDeviceId{.vendor = 0x1af4, .device = 0x1000, .class_code = 0x02},
    };
    if (shim.bind(device).code() != StatusCode::not_initialized)
    {
        return Status::fault("Linux driver shim accepted bind before driver registration");
    }

    driver::linux_compat::pci_driver incomplete{};
    if (shim.register_driver(incomplete).code() != StatusCode::invalid_argument)
    {
        return Status::fault("Linux driver shim accepted incomplete callbacks");
    }

    static const std::array ids{driver::linux_compat::pci_device_id{.vendor = 0x1af4, .device = 0x1000}};
    struct LinuxShimState
    {
        bool probed{false};
        bool removed{false};
    };
    static LinuxShimState shim_state{};
    shim_state = {};
    driver::linux_compat::pci_driver linux_driver{
        .name = "linux-fake-pci",
        .ids = ids,
        .probe =
            [](driver::OkDevice &, const driver::linux_compat::pci_device_id &) {
                shim_state.probed = true;
                return Status::success();
            },
        .remove =
            [](driver::OkDevice &) {
                shim_state.removed = true;
                return Status::success();
            },
    };
    if (auto status = shim.register_driver(linux_driver); !status.ok())
    {
        return status;
    }
    driver::OkDevice missing{
        .bus = driver::OkBusType::pci,
        .id = driver::OkDeviceId{.vendor = 0x1234, .device = 0x5678, .class_code = 0x02},
    };
    if (shim.bind(missing).code() != StatusCode::not_found)
    {
        return Status::fault("Linux driver shim matched the wrong PCI ID");
    }
    if (auto status = shim.bind(device); !status.ok())
    {
        return status;
    }
    if (!device.attached || shim.probe_count() != 1)
    {
        return Status::fault("Linux driver shim probe accounting failed");
    }

    auto *mmio = shim.ioremap(0x1000, 64);
    if (mmio == nullptr || !mmio->writel(0, 0xfeed).ok() || mmio->readl(0) != 0xfeed || !shim.iounmap(mmio).ok() ||
        mmio->writel(0, 0).code() != StatusCode::not_initialized)
    {
        return Status::fault("Linux driver shim MMIO validation failed");
    }
    if (shim.iounmap(nullptr).code() != StatusCode::invalid_argument)
    {
        return Status::fault("Linux driver shim accepted invalid MMIO unmap");
    }

    auto *allocation = shim.kmalloc(16);
    if (allocation == nullptr || shim.kmalloc(16) != nullptr || !shim.kfree(allocation).ok() ||
        shim.kfree(allocation).code() != StatusCode::invalid_argument)
    {
        return Status::fault("Linux driver shim allocation validation failed");
    }

    driver::OkSpinLock spinlock;
    if (!spinlock.try_lock())
    {
        return Status::fault("Linux driver shim spinlock validation failed");
    }
    spinlock.unlock();

    driver::OkIrqHandle irq{};
    if (!shim.request_irq(irq, 44).ok() || irq.vector != 44 ||
        shim.request_irq(irq, 45).code() != StatusCode::already_exists || !shim.free_irq(irq).ok() ||
        shim.free_irq(irq).code() != StatusCode::invalid_argument)
    {
        return Status::fault("Linux driver shim IRQ validation failed");
    }

    if (auto status = shim.remove(device); !status.ok())
    {
        return status;
    }
    if (!shim_state.probed || !shim_state.removed || device.attached || shim.remove_count() != 1)
    {
        return Status::fault("Linux driver shim probe/remove validation failed");
    }

    return Status::success();
}

} // namespace

Status run_driver_abi_roadmap_tests(KernelTestReport &report)
{
    if (auto status = test_driver_abi_helpers(); !status.ok())
    {
        return status;
    }
    if (auto status = test_driver_resource_manager_and_classes(); !status.ok())
    {
        return status;
    }
    if (auto status = test_native_driver_lifecycle(); !status.ok())
    {
        return status;
    }
    if (auto status = test_linux_driver_shim(); !status.ok())
    {
        return status;
    }

    report.driver_abi = true;
    report.linux_driver_shim = true;
    return Status::success();
}

} // namespace ok
