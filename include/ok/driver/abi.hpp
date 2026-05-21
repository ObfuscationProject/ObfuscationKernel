#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/driver/driver.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok::driver
{

inline constexpr usize max_ok_devices = 32;
inline constexpr usize max_ok_driver_modules = 16;
inline constexpr usize ok_mmio_words = 64;
inline constexpr usize ok_dma_buffer_size = 4096;

enum class OkBusType : u8
{
    pci,
    platform,
    virtio,
    usb,
};

struct OkDeviceId
{
    u32 vendor{0};
    u32 device{0};
    u32 class_code{0};
};

struct OkDevice
{
    OkBusType bus{OkBusType::pci};
    OkDeviceId id{};
    std::array<uptr, 6> resources{};
    bool attached{false};
};

struct OkDmaBuffer
{
    std::array<std::byte, ok_dma_buffer_size> bytes{};
    usize size{0};
};

struct OkIrqHandle
{
    u32 vector{0};
    bool registered{false};
};

struct OkMmioRegion
{
    std::array<u32, ok_mmio_words> words{};
    bool mapped{false};

    [[nodiscard]] u32 readl(usize index) const;
    Status writel(usize index, u32 value);
};

struct OkIoPortRegion
{
    u16 base{0};
    u16 length{0};
};

struct OkWorkQueue
{
    usize queued{0};
};

struct OkTimer
{
    u64 deadline_ticks{0};
    bool armed{false};
};

class OkRefCount final
{
  public:
    void get()
    {
        ++value_;
    }
    bool put()
    {
        if (value_ == 0)
        {
            return true;
        }
        --value_;
        return value_ == 0;
    }
    [[nodiscard]] usize value() const
    {
        return value_;
    }

  private:
    usize value_{1};
};

using OkSpinLock = smp::SpinLock;

class OkMutex final
{
  public:
    Status lock();
    Status unlock();
    [[nodiscard]] bool locked() const
    {
        return locked_;
    }

  private:
    bool locked_{false};
};

struct OkProbeContext
{
    OkDevice *device{nullptr};
    OkMmioRegion *mmio{nullptr};
    OkIrqHandle *irq{nullptr};
};

struct OkDriverManifest
{
    std::string_view name{};
    std::string_view version{};
    OkBusType bus{OkBusType::pci};
    std::span<const OkDeviceId> ids{};
};

struct OkDriverOps
{
    Status (*match)(const OkDevice &device, const OkDriverManifest &manifest){nullptr};
    Status (*probe)(OkProbeContext &context){nullptr};
    Status (*remove)(OkProbeContext &context){nullptr};
};

class OkDriverModule
{
  public:
    virtual ~OkDriverModule() = default;
    [[nodiscard]] virtual OkDriverManifest manifest() const = 0;
    virtual bool match(const OkDevice &device) const;
    virtual Status probe(OkProbeContext &context) = 0;
    virtual Status attach(OkProbeContext &context);
    virtual Status start();
    virtual Status suspend();
    virtual Status resume();
    virtual Status remove(OkProbeContext &context);
    virtual Status shutdown();
};

class OkDriverRegistry final
{
  public:
    Status register_device(OkDevice device);
    Status register_driver(OkDriverModule &driver);
    Status bind_all();
    Status remove_all();
    [[nodiscard]] usize device_count() const
    {
        return devices_.size();
    }
    [[nodiscard]] usize driver_count() const
    {
        return drivers_.size();
    }
    [[nodiscard]] usize bound_count() const
    {
        return bound_count_;
    }
    [[nodiscard]] OkMmioRegion &mmio()
    {
        return mmio_;
    }
    [[nodiscard]] OkIrqHandle &irq()
    {
        return irq_;
    }

  private:
    StaticVector<OkDevice, max_ok_devices> devices_;
    StaticVector<OkDriverModule *, max_ok_driver_modules> drivers_;
    OkMmioRegion mmio_{};
    OkIrqHandle irq_{};
    usize bound_count_{0};
};

namespace linux_compat
{

struct pci_device_id
{
    u32 vendor{0};
    u32 device{0};
};

struct pci_driver
{
    std::string_view name{};
    std::span<const pci_device_id> ids{};
    Status (*probe)(OkDevice &device, const pci_device_id &id){nullptr};
    Status (*remove)(OkDevice &device){nullptr};
};

class LinuxPciShim final
{
  public:
    Status register_driver(pci_driver &driver);
    Status bind(OkDevice &device);
    Status remove(OkDevice &device);
    [[nodiscard]] OkMmioRegion *ioremap(uptr, usize);
    Status iounmap(OkMmioRegion *region);
    [[nodiscard]] void *kmalloc(usize size);
    Status kfree(void *pointer);
    Status request_irq(OkIrqHandle &handle, u32 vector);
    Status free_irq(OkIrqHandle &handle);
    [[nodiscard]] usize probe_count() const
    {
        return probe_count_;
    }
    [[nodiscard]] usize remove_count() const
    {
        return remove_count_;
    }

  private:
    pci_driver *driver_{nullptr};
    OkMmioRegion mmio_{};
    std::array<std::byte, 256> heap_{};
    bool heap_used_{false};
    usize probe_count_{0};
    usize remove_count_{0};
};

} // namespace linux_compat

class NativeFakePciDriver final : public OkDriverModule
{
  public:
    [[nodiscard]] OkDriverManifest manifest() const override;
    Status probe(OkProbeContext &context) override;
    Status remove(OkProbeContext &context) override;
    [[nodiscard]] bool probed() const
    {
        return probed_;
    }
    [[nodiscard]] bool removed() const
    {
        return removed_;
    }

  private:
    bool probed_{false};
    bool removed_{false};
};

} // namespace ok::driver
