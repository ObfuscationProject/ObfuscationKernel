#include "ok/core/kernel.hpp"
#include "ok/core/test_point.hpp"
#include "ok/fs/ext4.hpp"

#include <array>

namespace ok
{
namespace
{

std::span<const memory::MemoryRegion> memory_map_span(const KernelConfig &config)
{
    return {config.memory_map.data(), config.memory_region_count};
}

std::span<const std::byte> as_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

std::string_view bounded_c_string(uptr address)
{
    if (address == 0)
    {
        return {};
    }
    const auto *text = reinterpret_cast<const char *>(address);
    usize size = 0;
    while (size < 255 && text[size] != '\0')
    {
        ++size;
    }
    return {text, size};
}

void write_le16(std::span<std::byte> out, usize offset, u16 value)
{
    out[offset] = static_cast<std::byte>(value & 0xffu);
    out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void write_le32(std::span<std::byte> out, usize offset, u32 value)
{
    write_le16(out, offset, static_cast<u16>(value & 0xffffu));
    write_le16(out, offset + 2, static_cast<u16>((value >> 16) & 0xffffu));
}

uptr test_mapping_address(arch::Architecture architecture)
{
    switch (architecture)
    {
    case arch::Architecture::i386:
    case arch::Architecture::arm32:
    case arch::Architecture::rv32:
    case arch::Architecture::mips:
    case arch::Architecture::ppc:
        return static_cast<uptr>(0xc000'0000u);
    case arch::Architecture::x86_64:
    case arch::Architecture::aarch64:
    case arch::Architecture::rv64:
    case arch::Architecture::loongarch64:
    case arch::Architecture::mips64:
    case arch::Architecture::ppc64:
        return static_cast<uptr>(0xffff'8000'0000'0000ull);
    }
    return static_cast<uptr>(0xc000'0000u);
}

} // namespace

Kernel::Kernel() : arch_(&arch::arch_operations(arch::configured_architecture()))
{
}

Status Kernel::boot(KernelConfig config)
{
    config.architecture = arch::configured_architecture();
    test_report_ = {};
    debug_test_points_run_ = 0;
    if (config.memory_region_count == 0)
    {
        config.memory_map[0] =
            memory::MemoryRegion{.base = 0x0010'0000, .size = 64 * 1024 * 1024, .type = memory::RegionType::usable};
        config.memory_region_count = 1;
    }

    config_ = config;
    arch_ = &arch::arch_operations(config_.architecture);
    memory_.set_translation_mode(config_.modes.memory);
    interrupts_.set_mode(config_.modes.interrupts);
    topology_.set_mode(config_.modes.smp);
    scheduler_.set_mode(config_.modes.scheduler);
    ipc_.set_mode(config_.modes.ipc);
    syscalls_.set_mode(config_.modes.syscalls);
    vfs_.set_mode(config_.modes.filesystem);
    user_space_.set_mode(config_.modes.user);
    keyboard_driver_.set_mode(config_.modes.drivers);
    mouse_driver_.set_mode(config_.modes.drivers);

    const auto hardware_threads = arch_->hardware_thread_count();
    if (auto status = topology_.initialize(hardware_threads == 0 ? 1 : hardware_threads); !status.ok())
    {
        return status;
    }
    if (auto status = topology_.mark_online(0); !status.ok())
    {
        return status;
    }
    for (usize cpu = 1; cpu < topology_.cpu_count(); ++cpu)
    {
        const auto cpu_id = static_cast<smp::CpuId>(cpu);
        if (auto status = topology_.mark_starting(cpu_id); !status.ok())
        {
            return status;
        }
        if (auto status = topology_.mark_online(cpu_id); !status.ok())
        {
            return status;
        }
    }
    if (auto status = scheduler_.configure_cpus(topology_.cpu_count()); !status.ok())
    {
        return status;
    }

    if (auto status = memory_.initialize(memory_map_span(config_), arch_->page_size()); !status.ok())
    {
        return status;
    }

    if (auto status = drivers_.add(console_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(timer_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(null_block_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(display_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(keyboard_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(mouse_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(pci_bus_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(usb_xhci_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(usb_keyboard_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(usb_mouse_driver_); !status.ok())
    {
        return status;
    }

    if (auto status = drivers_.start_all(); !status.ok())
    {
        return status;
    }
    if (auto status = log_boot_line("[    0.000000] okernel: C++23 kernel debug console online"); !status.ok())
    {
        return status;
    }
    if (auto status = log_boot_line("[    0.000001] arch: operations selected"); !status.ok())
    {
        return status;
    }
    if (auto status = log_boot_line("[    0.000002] smp: boot cpu online, secondary cpu records ready"); !status.ok())
    {
        return status;
    }
    if (auto status = log_boot_line("[    0.000003] memory: frame allocator and address space ready"); !status.ok())
    {
        return status;
    }
    if (auto status = log_boot_line("[    0.000004] driver: console timer block framebuffer pcie usb input started");
        !status.ok())
    {
        return status;
    }
    if (auto status = display_driver_.fill_rect(8, 76, 48, 12, 0xff44aa88u); !status.ok())
    {
        return status;
    }
    if (auto status = register_builtin_interrupts(timer_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = posix_.initialize(vfs_, console_driver_, scheduler_); !status.ok())
    {
        return status;
    }
    if (auto status = debug_shell_.attach(*this); !status.ok())
    {
        return status;
    }
    if (auto status = register_builtin_syscalls(posix_); !status.ok())
    {
        return status;
    }

    const auto idle_context = arch_->make_kernel_context(0x1000, 0x8000);
    auto idle = scheduler_.create_process("idle", idle_context);
    if (!idle)
    {
        return idle.status();
    }
    if (auto status = scheduler_.set_runnable(idle.value()); !status.ok())
    {
        return status;
    }
    if (!scheduler_.schedule_next())
    {
        return Status::fault("failed to schedule idle process");
    }
    if (auto status = topology_.record_schedule(0); !status.ok())
    {
        return status;
    }

    if (auto status = vfs_.create("/tmp/kernel.log", fs::NodeType::regular); !status.ok())
    {
        return status;
    }
    if (auto status = log_boot_line("[    0.000005] fs: ram vfs mounted at /"); !status.ok())
    {
        return status;
    }

    booted_ = true;
    return Status::success();
}

Status Kernel::run_debug_test_suite()
{
    if (!booted_)
    {
        return Status::not_initialized("kernel not booted");
    }

    auto frame = memory_.frames().allocate();
    if (!frame)
    {
        return frame.status();
    }
    const auto test_address = test_mapping_address(config_.architecture);
    if (auto status = memory_.kernel_address_space().map(test_address, frame.value(), 0b11); !status.ok())
    {
        return status;
    }
    if (auto status = memory_.kernel_address_space().unmap(test_address); !status.ok())
    {
        return status;
    }
    if (auto status = memory_.frames().release(frame.value()); !status.ok())
    {
        return status;
    }
    test_report_.memory = true;

    arch::TrapFrame trap{.vector = 32, .context = arch_->make_kernel_context(0x1000, 0x8000)};
    if (auto status = interrupts_.dispatch(trap); !status.ok())
    {
        return status;
    }
    test_report_.interrupts = true;

    auto channel = ipc_.create_channel();
    if (!channel)
    {
        return channel.status();
    }
    struct Payload
    {
        u32 value;
    };
    if (auto status = ipc_.send_value(channel.value(), scheduler_.current_pid(), scheduler_.current_pid(), Payload{42});
        !status.ok())
    {
        return status;
    }
    auto message = ipc_.receive(channel.value());
    if (!message || message.value().size != sizeof(Payload))
    {
        return Status::fault("IPC debug test failed");
    }
    test_report_.ipc = true;

    if (auto status = vfs_.write_file("/tmp/kernel.log", as_bytes("booted\n")); !status.ok())
    {
        return status;
    }
    auto log = vfs_.read_file("/tmp/kernel.log");
    if (!log || log.value().size != 7)
    {
        return Status::fault("VFS debug test failed");
    }
    test_report_.vfs = true;

    if (auto status = run_ext4_test(); !status.ok())
    {
        return status;
    }
    test_report_.ext4 = true;

    syscall::Request getpid{.number = syscall::Number::getpid, .caller = scheduler_.current_pid()};
    auto getpid_result = syscalls_.dispatch(getpid);
    if (!getpid_result.status.ok() || getpid_result.value != static_cast<i64>(scheduler_.current_pid()))
    {
        return Status::fault("getpid syscall debug test failed");
    }
    test_report_.syscalls = true;

    auto context = arch_->make_user_context(arch::UserEntry{
        .instruction_pointer = 0x400000,
        .stack_pointer = 0x800000,
        .argument = 7,
    });
    if (auto status = user_space_.enter_process(scheduler_.current_pid(),
                                                arch::UserEntry{
                                                    .instruction_pointer = 0x400000,
                                                    .stack_pointer = 0x800000,
                                                    .argument = 7,
                                                },
                                                context);
        !status.ok())
    {
        return status;
    }
    if (context.mode != arch::PrivilegeMode::user)
    {
        return Status::fault("user mode transition debug test failed");
    }
    test_report_.user_mode = true;

    if (display_driver_.text().empty() || display_driver_.checksum() == 0)
    {
        return Status::fault("display debug test failed");
    }
    test_report_.display = true;

    if (auto status = keyboard_driver_.feed_scancode(0x23); !status.ok())
    {
        return status;
    }
    auto key = keyboard_driver_.read_event();
    if (!key || !key.value().pressed || key.value().ascii != 'h')
    {
        return Status::fault("keyboard debug test failed");
    }
    if (auto status = mouse_driver_.feed_packet(driver::MousePacket{.delta_x = 3, .delta_y = -2, .left_button = true});
        !status.ok())
    {
        return status;
    }
    auto mouse = mouse_driver_.read_packet();
    if (!mouse || mouse.value().delta_x != 3 || mouse.value().delta_y != -2 || !mouse.value().left_button)
    {
        return Status::fault("mouse debug test failed");
    }
    test_report_.input = true;

    if (pci_bus_driver_.device_count() == 0 || pci_bus_driver_.find_class(0x0c, 0x03, 0x30) == nullptr)
    {
        return Status::fault("PCIe debug test failed");
    }
    test_report_.bus = true;

    if (usb_xhci_driver_.find_device(driver::UsbDeviceClass::hid, 1, 1) == nullptr ||
        usb_xhci_driver_.find_device(driver::UsbDeviceClass::hid, 1, 2) == nullptr)
    {
        return Status::fault("USB enumeration debug test failed");
    }
    if (auto status = usb_keyboard_driver_.feed_report(driver::UsbKeyboardReport{.keys = {0x0b, 0, 0, 0, 0, 0}});
        !status.ok())
    {
        return status;
    }
    auto usb_key = usb_keyboard_driver_.read_event();
    if (!usb_key || usb_key.value().ascii != 'h')
    {
        return Status::fault("USB HID keyboard debug test failed");
    }
    if (auto status = usb_mouse_driver_.feed_report(
            driver::UsbMouseReport{.buttons = 1, .delta_x = 5, .delta_y = -4, .wheel = 0});
        !status.ok())
    {
        return status;
    }
    auto usb_mouse = usb_mouse_driver_.read_packet();
    if (!usb_mouse || usb_mouse.value().delta_x != 5 || usb_mouse.value().delta_y != -4 ||
        !usb_mouse.value().left_button)
    {
        return Status::fault("USB HID mouse debug test failed");
    }
    test_report_.usb = true;

    auto fd = posix_.open("/tmp/posix.txt", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC);
    if (!fd)
    {
        return fd.status();
    }
    constexpr std::string_view posix_text{"posix"};
    auto written = posix_.write(fd.value(), as_bytes(posix_text));
    if (!written || written.value() != posix_text.size())
    {
        return Status::fault("POSIX write debug test failed");
    }
    if (auto seek = posix_.seek(fd.value(), 0, posix::SeekWhence::set); !seek)
    {
        return seek.status();
    }
    std::array<std::byte, 8> posix_buffer{};
    auto read = posix_.read(fd.value(), posix_buffer);
    if (!read || read.value() != posix_text.size())
    {
        return Status::fault("POSIX read debug test failed");
    }
    auto posix_stat = posix_.stat("/tmp/posix.txt");
    if (!posix_stat || posix_stat.value().size != posix_text.size())
    {
        return Status::fault("POSIX stat debug test failed");
    }
    if (auto status = posix_.close(fd.value()); !status.ok())
    {
        return status;
    }
    if (auto status = posix_.chdir("/tmp"); !status.ok())
    {
        return status;
    }
    auto relative_fd = posix_.open("posix-relative.txt", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC);
    if (!relative_fd)
    {
        return relative_fd.status();
    }
    if (auto status = posix_.close(relative_fd.value()); !status.ok())
    {
        return status;
    }
    if (auto relative_stat = posix_.stat("/tmp/posix-relative.txt"); !relative_stat)
    {
        return Status::fault("POSIX relative path debug test failed");
    }
    if (auto status = posix_.chdir("/"); !status.ok())
    {
        return status;
    }
    test_report_.posix = true;

    auto shell_status = debug_shell_.execute("status");
    auto shell_posix = debug_shell_.execute("posix");
    if (!shell_status || shell_status.value().empty() || !shell_posix || shell_posix.value().empty())
    {
        return Status::fault("debug shell test failed");
    }
    test_report_.shell = true;

    if (memory_.translation_mode() != config_.modes.memory || interrupts_.mode() != config_.modes.interrupts ||
        topology_.mode() != config_.modes.smp || scheduler_.mode() != config_.modes.scheduler ||
        ipc_.mode() != config_.modes.ipc || syscalls_.mode() != config_.modes.syscalls ||
        vfs_.mode() != config_.modes.filesystem || user_space_.mode() != config_.modes.user ||
        keyboard_driver_.mode() != config_.modes.drivers || mouse_driver_.mode() != config_.modes.drivers)
    {
        return Status::fault("module mode propagation failed");
    }
    test_report_.modes = true;

    auto debug_test_points = test::run_kernel_test_points(*this);
    if (!debug_test_points)
    {
        return debug_test_points.status();
    }
    debug_test_points_run_ = debug_test_points.value();

    return Status::success();
}

Status Kernel::log_boot_line(std::string_view line)
{
    if (auto status = console_driver_.write(line); !status.ok())
    {
        return status;
    }
    if (auto status = console_driver_.write("\n"); !status.ok())
    {
        return status;
    }
    return display_driver_.write_line(line);
}

Status Kernel::run_ext4_test()
{
    std::array<std::byte, 4096> image{};
    auto bytes = std::span<std::byte>(image.data(), image.size());
    const auto base = fs::ext4_superblock_offset;
    write_le32(bytes, base + 0x00, 128);
    write_le32(bytes, base + 0x04, 4);
    write_le32(bytes, base + 0x0c, 2);
    write_le32(bytes, base + 0x18, 0);
    write_le16(bytes, base + 0x38, fs::ext4_superblock_magic);
    write_le16(bytes, base + 0x58, 256);
    write_le32(bytes, base + 0x60, 0x40);

    constexpr std::string_view name{"OKEXT4"};
    for (usize i = 0; i < name.size(); ++i)
    {
        bytes[base + 0x78 + i] = static_cast<std::byte>(name[i]);
    }

    fs::Ext4Volume volume;
    if (auto status = volume.mount(image); !status.ok())
    {
        return status;
    }
    auto info = volume.info();
    if (!info || info.value().block_size != 1024 || info.value().inode_size != 256 ||
        info.value().volume_name.view() != name || !info.value().has_extents)
    {
        return Status::fault("EXT4 debug test failed");
    }

    std::array<std::byte, 1024> block{};
    return volume.read_block(1, block);
}

Status Kernel::register_builtin_interrupts(driver::TimerDriver &timer)
{
    return interrupts_.register_callback(32, "timer", &timer, [](void *context, arch::TrapFrame &) {
        static_cast<driver::TimerDriver *>(context)->tick();
        return Status::success();
    });
}

Status Kernel::register_builtin_syscalls(posix::PosixService &posix)
{
    if (auto status = syscalls_.register_callback(
            syscall::Number::getpid, "getpid", &posix,
            [](void *, const syscall::Request &request) {
                return syscall::Response{.value = static_cast<i64>(request.caller), .status = Status::success()};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::read, "read", &posix,
            [](void *context, const syscall::Request &request) {
                auto *service = static_cast<posix::PosixService *>(context);
                auto *buffer = reinterpret_cast<std::byte *>(request.args[1]);
                auto result = service->read(static_cast<posix::Fd>(request.args[0]),
                                            std::span<std::byte>{buffer, static_cast<usize>(request.args[2])});
                return syscall::Response{.value = result ? static_cast<i64>(result.value()) : -1,
                                         .status = result ? Status::success() : result.status()};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::write, "write", &posix,
            [](void *context, const syscall::Request &request) {
                if (request.args[1] == 0 && request.args[2] != 0)
                {
                    return syscall::Response{.value = -1, .status = Status::invalid_argument("write buffer is empty")};
                }
                auto *service = static_cast<posix::PosixService *>(context);
                const auto *bytes = reinterpret_cast<const std::byte *>(request.args[1]);
                auto result = service->write(static_cast<posix::Fd>(request.args[0]),
                                             std::span<const std::byte>{bytes, static_cast<usize>(request.args[2])});
                return syscall::Response{.value = result ? static_cast<i64>(result.value()) : -1,
                                         .status = result ? Status::success() : result.status()};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::open, "open", &posix,
            [](void *context, const syscall::Request &request) {
                auto *service = static_cast<posix::PosixService *>(context);
                auto result = service->open(bounded_c_string(request.args[0]), static_cast<u32>(request.args[1]),
                                            static_cast<u32>(request.args[2]));
                return syscall::Response{.value = result ? static_cast<i64>(result.value()) : -1,
                                         .status = result ? Status::success() : result.status()};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::close, "close", &posix,
            [](void *context, const syscall::Request &request) {
                auto status = static_cast<posix::PosixService *>(context)->close(static_cast<posix::Fd>(request.args[0]));
                return syscall::Response{.value = status.ok() ? 0 : -1, .status = status};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::stat, "stat", &posix,
            [](void *context, const syscall::Request &request) {
                auto result = static_cast<posix::PosixService *>(context)->stat(bounded_c_string(request.args[0]));
                if (result && request.args[1] != 0)
                {
                    *reinterpret_cast<posix::FileStatus *>(request.args[1]) = result.value();
                }
                return syscall::Response{.value = result ? 0 : -1, .status = result ? Status::success() : result.status()};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::mkdir, "mkdir", &posix,
            [](void *context, const syscall::Request &request) {
                auto status = static_cast<posix::PosixService *>(context)->mkdir(bounded_c_string(request.args[0]),
                                                                                 static_cast<u32>(request.args[1]));
                return syscall::Response{.value = status.ok() ? 0 : -1, .status = status};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::unlink, "unlink", &posix,
            [](void *context, const syscall::Request &request) {
                auto status = static_cast<posix::PosixService *>(context)->unlink(bounded_c_string(request.args[0]));
                return syscall::Response{.value = status.ok() ? 0 : -1, .status = status};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::chdir, "chdir", &posix,
            [](void *context, const syscall::Request &request) {
                auto status = static_cast<posix::PosixService *>(context)->chdir(bounded_c_string(request.args[0]));
                return syscall::Response{.value = status.ok() ? 0 : -1, .status = status};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::getcwd, "getcwd", &posix,
            [](void *context, const syscall::Request &request) {
                auto cwd = static_cast<posix::PosixService *>(context)->getcwd();
                auto *out = reinterpret_cast<char *>(request.args[0]);
                const auto capacity = static_cast<usize>(request.args[1]);
                if (out == nullptr || capacity <= cwd.size())
                {
                    return syscall::Response{.value = -1, .status = Status::overflow("getcwd buffer too small")};
                }
                for (usize i = 0; i < cwd.size(); ++i)
                {
                    out[i] = cwd[i];
                }
                out[cwd.size()] = '\0';
                return syscall::Response{.value = static_cast<i64>(cwd.size()), .status = Status::success()};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::uname, "uname", &posix,
            [](void *context, const syscall::Request &request) {
                if (request.args[0] == 0)
                {
                    return syscall::Response{.value = -1, .status = Status::invalid_argument("uname buffer is null")};
                }
                *reinterpret_cast<posix::UnameInfo *>(request.args[0]) =
                    static_cast<posix::PosixService *>(context)->uname();
                return syscall::Response{.value = 0, .status = Status::success()};
            });
        !status.ok())
    {
        return status;
    }

    if (auto status = syscalls_.register_callback(
            syscall::Number::clock_gettime, "clock_gettime", &posix,
            [](void *context, const syscall::Request &request) {
                if (request.args[1] == 0)
                {
                    return syscall::Response{.value = -1, .status = Status::invalid_argument("clock buffer is null")};
                }
                *reinterpret_cast<posix::ClockTime *>(request.args[1]) =
                    static_cast<posix::PosixService *>(context)->clock_gettime();
                return syscall::Response{.value = 0, .status = Status::success()};
            });
        !status.ok())
    {
        return status;
    }

    return syscalls_.register_callback(
        syscall::Number::ok_debug, "ok_debug", nullptr, [](void *, const syscall::Request &request) {
            return syscall::Response{.value = static_cast<i64>(request.args[0]), .status = Status::success()};
        });
}

} // namespace ok
