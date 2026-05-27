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
inline constexpr usize max_ok_resources = 32;
inline constexpr usize ok_mmio_words = 64;
inline constexpr usize ok_dma_buffer_size = 4096;
inline constexpr usize max_ok_dma_mappings = 16;
inline constexpr usize max_ok_work_items = 32;
inline constexpr usize max_ok_slab_allocations = 16;
inline constexpr usize max_ok_waiters = 16;
inline constexpr usize max_ok_kernel_threads = 16;
inline constexpr usize max_ok_thread_name = 48;

enum class OkBusType : u8
{
    pci,
    platform,
    virtio,
    usb,
};

enum class OkDeviceClass : u8
{
    generic,
    block,
    network,
    input,
    display,
};

enum class OkResourceKind : u8
{
    mmio,
    io_port,
    irq,
    dma,
};

enum class OkDmaDirection : u8
{
    to_device,
    from_device,
    bidirectional,
};

struct OkResource
{
    OkResourceKind kind{OkResourceKind::mmio};
    uptr base{0};
    usize length{0};
    u32 flags{0};
    bool claimed{false};
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
    OkDeviceClass device_class{OkDeviceClass::generic};
};

struct OkDmaBuffer
{
    std::array<std::byte, ok_dma_buffer_size> bytes{};
    usize size{0};
};

struct OkDmaMapping
{
    uptr device_address{0};
    usize size{0};
    OkDmaDirection direction{OkDmaDirection::bidirectional};
    bool mapped{false};
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

    Status enqueue();
    Result<usize> drain();
};

struct OkTimer
{
    u64 deadline_ticks{0};
    bool armed{false};

    Status arm(u64 deadline);
    Status cancel();
};

class OkDmaMapper final
{
  public:
    Result<OkDmaMapping> map(OkDmaBuffer &buffer, usize size, OkDmaDirection direction);
    Status unmap(OkDmaMapping &mapping);
    [[nodiscard]] usize mapping_count() const
    {
        return mapping_count_;
    }

  private:
    std::array<OkDmaMapping, max_ok_dma_mappings> mappings_{};
    std::array<bool, max_ok_dma_mappings> used_{};
    usize mapping_count_{0};
};

class OkResourceManager final
{
  public:
    Status add(OkResource resource);
    Result<OkResource *> claim(OkResourceKind kind, uptr base, usize length);
    Status release(OkResourceKind kind, uptr base);
    [[nodiscard]] usize resource_count() const
    {
        return resources_.size();
    }
    [[nodiscard]] usize claimed_count() const;
    [[nodiscard]] const OkResource *find(OkResourceKind kind, uptr base) const;

  private:
    StaticVector<OkResource, max_ok_resources> resources_;
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

class OkWaitQueue final
{
  public:
    Status wait(u32 token);
    Status wake_one();
    Status wake_all();
    [[nodiscard]] bool waiting(u32 token) const;
    [[nodiscard]] usize waiter_count() const
    {
        return waiters_.size();
    }

  private:
    StaticVector<u32, max_ok_waiters> waiters_;
};

class OkSlabAllocator final
{
  public:
    Result<void *> allocate(usize size);
    Status free(void *pointer);
    [[nodiscard]] usize allocation_count() const
    {
        return allocation_count_;
    }

  private:
    std::array<std::array<std::byte, 256>, max_ok_slab_allocations> slots_{};
    std::array<bool, max_ok_slab_allocations> used_{};
    usize allocation_count_{0};
};

struct OkKernelThread
{
    u32 id{0};
    FixedString<max_ok_thread_name> name{};
    bool running{false};
};

class OkKernelThreadRegistry final
{
  public:
    Result<u32> create(std::string_view name);
    Status stop(u32 id);
    [[nodiscard]] const OkKernelThread *find(u32 id) const;
    [[nodiscard]] usize thread_count() const
    {
        return threads_.size();
    }

  private:
    StaticVector<OkKernelThread, max_ok_kernel_threads> threads_;
    u32 next_id_{1};
};

struct OkProbeContext
{
    OkDevice *device{nullptr};
    OkMmioRegion *mmio{nullptr};
    OkIrqHandle *irq{nullptr};
    OkResourceManager *resources{nullptr};
    OkDmaMapper *dma{nullptr};
    OkWorkQueue *workqueue{nullptr};
};

struct OkDriverManifest
{
    std::string_view name{};
    std::string_view version{};
    OkBusType bus{OkBusType::pci};
    std::span<const OkDeviceId> ids{};
    OkDeviceClass device_class{OkDeviceClass::generic};
};

struct OkBlockRequest
{
    u64 sector{0};
    std::span<std::byte> buffer{};
};

struct OkNetBuffer
{
    std::span<std::byte> frame{};
};

class OkBlockOperations
{
  public:
    virtual ~OkBlockOperations() = default;
    virtual Result<usize> read(OkBlockRequest request) = 0;
    virtual Result<usize> write(OkBlockRequest request) = 0;
};

class OkNetworkOperations
{
  public:
    virtual ~OkNetworkOperations() = default;
    virtual Status transmit(OkNetBuffer buffer) = 0;
    virtual Result<usize> receive(OkNetBuffer buffer) = 0;
};

class OkInputOperations
{
  public:
    virtual ~OkInputOperations() = default;
    virtual Result<u32> poll_event() = 0;
};

class OkDisplayOperations
{
  public:
    virtual ~OkDisplayOperations() = default;
    virtual Status present(std::span<const std::byte> frame) = 0;
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
    [[nodiscard]] OkResourceManager &resources()
    {
        return resources_;
    }
    [[nodiscard]] OkDmaMapper &dma()
    {
        return dma_;
    }
    [[nodiscard]] OkWorkQueue &workqueue()
    {
        return workqueue_;
    }

  private:
    StaticVector<OkDevice, max_ok_devices> devices_;
    StaticVector<OkDriverModule *, max_ok_driver_modules> drivers_;
    OkMmioRegion mmio_{};
    OkIrqHandle irq_{};
    OkResourceManager resources_{};
    OkDmaMapper dma_{};
    OkWorkQueue workqueue_{};
    usize bound_count_{0};
};

[[nodiscard]] OkDeviceClass ok_device_class_for(const OkDevice &device);
[[nodiscard]] std::string_view ok_device_class_name(OkDeviceClass device_class);

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
    Result<OkDmaMapping> dma_map_single(OkDmaBuffer &buffer, usize size, OkDmaDirection direction);
    Status dma_unmap_single(OkDmaMapping &mapping);
    Status schedule_work();
    Result<usize> flush_work();
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
    OkDmaMapper dma_{};
    OkWorkQueue workqueue_{};
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
