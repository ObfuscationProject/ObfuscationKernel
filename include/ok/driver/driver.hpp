#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/concepts.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/sched/scheduler.hpp"

#include <array>
#include <concepts>
#include <span>
#include <string_view>

namespace ok::driver
{

enum class Class : u8
{
    console,
    timer,
    block,
    display,
    network,
    input,
    bus,
    usb,
    entropy,
};

class Driver
{
  public:
    virtual ~Driver() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual Class driver_class() const = 0;
    virtual Status probe() = 0;
    virtual Status start() = 0;
    virtual Status stop() = 0;
};

template <typename T>
concept KernelDriver = std::derived_from<T, Driver>;

inline constexpr usize max_drivers = 32;
inline constexpr usize console_buffer_size = 4096;
inline constexpr usize framebuffer_width = 480;
inline constexpr usize framebuffer_height = 270;
inline constexpr usize framebuffer_pixels = framebuffer_width * framebuffer_height;
inline constexpr usize display_text_columns = 80;
inline constexpr usize display_text_rows = 25;
inline constexpr usize display_text_buffer_size = (display_text_columns + 1) * display_text_rows;
inline constexpr usize input_queue_capacity = 128;
inline constexpr usize max_pci_devices = 32;
inline constexpr usize max_usb_devices = 32;
inline constexpr usize block_sector_size = 512;
inline constexpr usize ram_block_sector_count = 512;
inline constexpr usize ram_block_storage_size = block_sector_size * ram_block_sector_count;
inline constexpr usize virtio_block_sector_count = 8192;
inline constexpr usize virtio_block_storage_size = block_sector_size * virtio_block_sector_count;

enum class IoMode : u8
{
    polling,
    interrupt,
    dma,
};

class DriverManager final
{
  public:
    struct DriverProcessRestart
    {
        FixedString<sched::max_process_name> process_name{};
        sched::ProcessId previous_pid{0};
        sched::ProcessId pid{0};
    };

    Status add(Driver &driver);

    Status start_all();
    Status bind_kernel_processes(sched::Scheduler &scheduler, arch::ArchOperations &arch, uptr entry_base,
                                 uptr stack_base);
    Status supervise_kernel_processes(StaticVector<DriverProcessRestart, max_drivers> &restarts);
    [[nodiscard]] Driver *find(Class driver_class);
    [[nodiscard]] Result<sched::ProcessId> kernel_process_id(std::string_view driver_name) const;
    [[nodiscard]] usize driver_count() const
    {
        return drivers_.size();
    }
    [[nodiscard]] usize kernel_process_count() const
    {
        return driver_processes_.size();
    }

  private:
    struct DriverProcessRecord
    {
        Driver *driver{nullptr};
        sched::ProcessId pid{0};
    };

    [[nodiscard]] DriverProcessRecord *find_process_record(Driver &driver);
    [[nodiscard]] const DriverProcessRecord *find_process_record(const Driver &driver) const;
    Result<sched::ProcessId> ensure_kernel_process(Driver &driver, usize driver_index);

    StaticVector<Driver *, max_drivers> drivers_;
    StaticVector<DriverProcessRecord, max_drivers> driver_processes_;
    sched::Scheduler *kernel_process_scheduler_{nullptr};
    arch::ArchOperations *kernel_process_arch_{nullptr};
    uptr kernel_process_entry_base_{0};
    uptr kernel_process_stack_base_{0};
};

class ConsoleDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "kernel-console";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::console;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status write(std::string_view text);
    [[nodiscard]] std::string_view buffer() const
    {
        return {buffer_.data(), buffer_size_};
    }

  private:
    bool started_{false};
    std::array<char, console_buffer_size> buffer_{};
    usize buffer_size_{0};
};

class TimerDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "kernel-timer";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::timer;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    void tick()
    {
        ++ticks_;
    }
    [[nodiscard]] u64 ticks() const
    {
        return ticks_;
    }

  private:
    bool started_{false};
    u64 ticks_{0};
};

struct BlockGeometry
{
    u64 block_count{0};
    u32 block_size{block_sector_size};
    bool writable{false};
};

struct BlockIoStats
{
    u64 read_operations{0};
    u64 write_operations{0};
    u64 bytes_read{0};
    u64 bytes_written{0};
    u64 errors{0};
};

class BlockDevice
{
  public:
    virtual ~BlockDevice() = default;
    [[nodiscard]] virtual BlockGeometry geometry() const = 0;
    virtual Status read_blocks(u64 first_block, std::span<std::byte> out) = 0;
    virtual Status write_blocks(u64 first_block, std::span<const std::byte> in) = 0;
    [[nodiscard]] virtual BlockIoStats io_stats() const
    {
        return {};
    }
};

class NullBlockDriver final : public Driver, public BlockDevice
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "null-block";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::block;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    [[nodiscard]] BlockGeometry geometry() const override;
    Status read_blocks(u64 first_block, std::span<std::byte> out) override;
    Status write_blocks(u64 first_block, std::span<const std::byte> in) override;
    [[nodiscard]] BlockIoStats io_stats() const override
    {
        return stats_;
    }
    Status read(uptr, std::span<std::byte> out);
    Status write(uptr, std::span<const std::byte> in);

  private:
    bool started_{false};
    BlockIoStats stats_{};
};

class RamBlockDriver final : public Driver, public BlockDevice
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "ram-block0";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::block;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    [[nodiscard]] BlockGeometry geometry() const override;
    Status read_blocks(u64 first_block, std::span<std::byte> out) override;
    Status write_blocks(u64 first_block, std::span<const std::byte> in) override;
    [[nodiscard]] BlockIoStats io_stats() const override
    {
        return stats_;
    }
    Status clear();

  private:
    [[nodiscard]] Status check_transfer(u64 first_block, usize byte_count) const;

    bool started_{false};
    std::array<std::byte, ram_block_storage_size> storage_{};
    BlockIoStats stats_{};
};

struct PciDeviceId
{
    u16 vendor_id{0xffff};
    u16 device_id{0xffff};
    u8 class_code{0};
    u8 subclass{0};
    u8 programming_interface{0};
};

struct PciDevice
{
    u8 bus{0};
    u8 slot{0};
    u8 function{0};
    PciDeviceId id{};
    std::array<uptr, 6> bars{};
};

class VirtioBlockDriver final : public Driver, public BlockDevice
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "virtio-blk0";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::block;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status bind(const PciDevice &device);
    [[nodiscard]] BlockGeometry geometry() const override;
    Status read_blocks(u64 first_block, std::span<std::byte> out) override;
    Status write_blocks(u64 first_block, std::span<const std::byte> in) override;
    [[nodiscard]] BlockIoStats io_stats() const override
    {
        return stats_;
    }
    Status clear();
    [[nodiscard]] bool bound() const
    {
        return bound_;
    }

  private:
    [[nodiscard]] Status check_transfer(u64 first_block, usize byte_count) const;

    bool started_{false};
    bool bound_{false};
    PciDevice device_{};
    std::array<std::byte, virtio_block_storage_size> storage_{};
    BlockIoStats stats_{};
};

class PciBusDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "pcie-root-bus";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::bus;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status add_emulated_device(PciDevice device);
    [[nodiscard]] const PciDevice *find_class(u8 class_code, u8 subclass, u8 programming_interface) const;
    [[nodiscard]] usize device_count() const
    {
        return devices_.size();
    }

  private:
    bool started_{false};
    StaticVector<PciDevice, max_pci_devices> devices_{};
};

struct KeyEvent
{
    u8 scancode{0};
    char ascii{0};
    bool pressed{false};
};

class KeyboardDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "ps2-keyboard";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::input;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status feed_scancode(u8 scancode);
    Result<KeyEvent> read_event();
    [[nodiscard]] IoMode mode() const
    {
        return mode_;
    }
    void set_mode(IoMode mode)
    {
        mode_ = mode;
    }
    [[nodiscard]] usize queued() const
    {
        return events_.size();
    }

  private:
    [[nodiscard]] char translate(u8 scancode) const;

    bool started_{false};
    bool left_shift_{false};
    bool right_shift_{false};
    IoMode mode_{IoMode::polling};
    StaticQueue<KeyEvent, input_queue_capacity> events_{};
};

struct MousePacket
{
    i8 delta_x{0};
    i8 delta_y{0};
    bool left_button{false};
    bool right_button{false};
    bool middle_button{false};
};

class Ps2MouseDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "ps2-mouse";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::input;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status feed_packet(MousePacket packet);
    Result<MousePacket> read_packet();
    [[nodiscard]] IoMode mode() const
    {
        return mode_;
    }
    void set_mode(IoMode mode)
    {
        mode_ = mode;
    }

  private:
    bool started_{false};
    IoMode mode_{IoMode::polling};
    StaticQueue<MousePacket, input_queue_capacity> packets_{};
};

enum class UsbSpeed : u8
{
    low,
    full,
    high,
    super,
};

enum class UsbDeviceClass : u8
{
    hid = 0x03,
    mass_storage = 0x08,
    hub = 0x09,
    vendor = 0xff,
};

struct UsbDevice
{
    u8 address{0};
    UsbSpeed speed{UsbSpeed::full};
    UsbDeviceClass device_class{UsbDeviceClass::vendor};
    u8 subclass{0};
    u8 protocol{0};
};

struct UsbKeyboardReport
{
    u8 modifiers{0};
    std::array<u8, 6> keys{};
};

struct UsbMouseReport
{
    u8 buttons{0};
    i8 delta_x{0};
    i8 delta_y{0};
    i8 wheel{0};
};

class UsbXhciControllerDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "xhci-usb-controller";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::usb;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status attach_device(UsbDevice device);
    [[nodiscard]] const UsbDevice *find_device(UsbDeviceClass device_class, u8 subclass, u8 protocol) const;
    [[nodiscard]] usize device_count() const
    {
        return devices_.size();
    }

  private:
    bool started_{false};
    StaticVector<UsbDevice, max_usb_devices> devices_{};
};

class UsbHidKeyboardDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "usb-hid-keyboard";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::input;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status feed_report(UsbKeyboardReport report);
    Result<KeyEvent> read_event();

  private:
    [[nodiscard]] char translate_usage(u8 usage, bool shift) const;

    bool started_{false};
    StaticQueue<KeyEvent, input_queue_capacity> events_{};
};

class UsbHidMouseDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "usb-hid-mouse";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::input;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status feed_report(UsbMouseReport report);
    Result<MousePacket> read_packet();

  private:
    bool started_{false};
    StaticQueue<MousePacket, input_queue_capacity> packets_{};
};

struct DisplayMode
{
    u32 width{framebuffer_width};
    u32 height{framebuffer_height};
    u32 pitch{framebuffer_width * sizeof(u32)};
    u8 bits_per_pixel{32};
};

enum class DisplayBackend : u8
{
    memory_framebuffer,
    vga_text,
    ramfb,
    virtio_gpu_pci,
};

class FramebufferDisplayDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "simple-framebuffer";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::display;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status clear(u32 rgba);
    Status put_pixel(u32 x, u32 y, u32 rgba);
    Status present_gui_pixel(u32 logical_width, u32 logical_height, u32 x, u32 y, u32 rgba);
    Status fill_rect(u32 x, u32 y, u32 width, u32 height, u32 rgba);
    Status write_line(std::string_view text);
    [[nodiscard]] Result<u32> pixel_at(u32 x, u32 y) const;
    void set_backend(DisplayBackend backend)
    {
        backend_ = backend;
    }
    [[nodiscard]] DisplayBackend backend() const
    {
        return backend_;
    }
    [[nodiscard]] DisplayMode mode() const
    {
        return mode_;
    }
    [[nodiscard]] std::string_view text() const
    {
        return {text_.data(), text_size_};
    }
    [[nodiscard]] u64 checksum() const;

  private:
    void draw_cell(u32 column, u32 row, char value);

    bool started_{false};
    DisplayBackend backend_{DisplayBackend::memory_framebuffer};
    DisplayMode mode_{};
    std::array<u32, framebuffer_pixels> pixels_{};
    std::array<char, display_text_buffer_size> text_{};
    usize text_size_{0};
    u32 cursor_row_{0};
};

class VirtioGpuPciDisplayDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "virtio-gpu-pci";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::display;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status bind(const PciDevice &device);
    Status present(const FramebufferDisplayDriver &framebuffer);
    [[nodiscard]] bool bound() const
    {
        return bound_;
    }
    [[nodiscard]] u64 frames_presented() const
    {
        return frames_presented_;
    }

  private:
    bool started_{false};
    bool bound_{false};
    PciDevice device_{};
    u64 frames_presented_{0};
};

} // namespace ok::driver
