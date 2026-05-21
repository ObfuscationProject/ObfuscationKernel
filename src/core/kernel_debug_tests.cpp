#include "ok/core/kernel.hpp"
#include "ok/core/module.hpp"
#include "ok/core/test_point.hpp"
#include "ok/driver/abi.hpp"
#include "ok/fs/ext4.hpp"
#include "ok/fs/storage.hpp"
#include "ok/fs/unix.hpp"
#include "ok/sched/process.hpp"
#include "ok/smp/preempt.hpp"
#include "ok/syscall/linux.hpp"

#include <array>
#include <span>

namespace ok
{
namespace
{

std::span<const std::byte> as_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
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

void write_le64(std::span<std::byte> out, usize offset, u64 value)
{
    write_le32(out, offset, static_cast<u32>(value & 0xffff'ffffu));
    write_le32(out, offset + 4, static_cast<u32>((value >> 32) & 0xffff'ffffu));
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
        return static_cast<uptr>(0xffff'8000'0000'0000ull);
    }
    return static_cast<uptr>(0xc000'0000u);
}

class DebugModule final : public KernelModule
{
  public:
    DebugModule(std::string_view name, std::string_view klass, std::span<const ModuleDependency> dependencies,
                std::span<const std::string_view> exports, std::span<const std::string_view> required_services,
                u32 priority,
                usize *start_sequence = nullptr, usize *stop_sequence = nullptr)
        : name_(name), klass_(klass), dependencies_(dependencies), exports_(exports), requires_(required_services),
          priority_(priority), start_sequence_(start_sequence), stop_sequence_(stop_sequence)
    {
    }

    [[nodiscard]] ModuleManifest manifest() const override
    {
        return ModuleManifest{
            .name = name_.view(),
            .version = "1",
            .module_class = klass_.view(),
            .dependencies = dependencies_,
            .exported_services = exports_,
            .required_services = requires_,
            .built_in = true,
            .init_priority = priority_,
        };
    }

    Status start(ServiceRegistry &) override
    {
        if (start_sequence_ != nullptr)
        {
            started_at_ = ++(*start_sequence_);
        }
        return Status::success();
    }

    Status stop() override
    {
        if (stop_sequence_ != nullptr)
        {
            stopped_at_ = ++(*stop_sequence_);
        }
        return Status::success();
    }

    [[nodiscard]] usize started_at() const
    {
        return started_at_;
    }
    [[nodiscard]] usize stopped_at() const
    {
        return stopped_at_;
    }

  private:
    FixedString<max_module_name> name_{};
    FixedString<max_module_name> klass_{};
    std::span<const ModuleDependency> dependencies_{};
    std::span<const std::string_view> exports_{};
    std::span<const std::string_view> requires_{};
    u32 priority_{0};
    usize *start_sequence_{nullptr};
    usize *stop_sequence_{nullptr};
    usize started_at_{0};
    usize stopped_at_{0};
};

std::array<std::byte, 128> make_test_elf(arch::Architecture architecture)
{
    std::array<std::byte, 128> image{};
    auto bytes = std::span<std::byte>(image.data(), image.size());
    bytes[0] = std::byte{0x7f};
    bytes[1] = std::byte{'E'};
    bytes[2] = std::byte{'L'};
    bytes[3] = std::byte{'F'};
    bytes[4] = std::byte{2};
    bytes[5] = std::byte{1};
    write_le16(bytes, 16, 2);
    u16 machine = 0x3e;
    switch (architecture)
    {
    case arch::Architecture::i386:
        machine = 0x03;
        break;
    case arch::Architecture::aarch64:
        machine = 0xb7;
        break;
    case arch::Architecture::arm32:
        machine = 0x28;
        break;
    case arch::Architecture::rv64:
    case arch::Architecture::rv32:
        machine = 0xf3;
        break;
    case arch::Architecture::mips:
    case arch::Architecture::mips64:
        machine = 0x08;
        break;
    case arch::Architecture::ppc:
        machine = 0x14;
        break;
    case arch::Architecture::loongarch64:
        machine = 0x102;
        break;
    case arch::Architecture::x86_64:
        machine = 0x3e;
        break;
    }
    write_le16(bytes, 18, machine);
    write_le64(bytes, 24, 0x400000);
    write_le64(bytes, 32, 64);
    write_le16(bytes, 54, 56);
    write_le16(bytes, 56, 1);
    return image;
}

Status run_module_roadmap_tests(KernelTestReport &report)
{
    usize start_sequence = 0;
    usize stop_sequence = 0;
    constexpr std::array<std::string_view, 1> arch_exports{"arch.ops"};
    constexpr std::array<std::string_view, 1> memory_exports{"memory.frames"};
    constexpr std::array<std::string_view, 1> vfs_exports{"vfs.root"};
    constexpr std::array<std::string_view, 1> required_arch{"arch.ops"};
    constexpr std::array<ModuleDependency, 1> depends_arch{ModuleDependency{.name = "arch", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_interrupt{
        ModuleDependency{.name = "interrupt", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_scheduler{
        ModuleDependency{.name = "scheduler", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_driver{ModuleDependency{.name = "driver-core", .required = true}};
    constexpr std::array<ModuleDependency, 1> depends_vfs{ModuleDependency{.name = "vfs", .required = true}};

    DebugModule arch_module{"arch", "architecture", {}, arch_exports, {}, 0, &start_sequence, &stop_sequence};
    DebugModule memory_module{"memory", "memory", depends_arch, memory_exports, required_arch, 10, &start_sequence,
                              &stop_sequence};
    DebugModule interrupt_module{"interrupt", "interrupt", depends_arch, {}, {}, 20, &start_sequence, &stop_sequence};
    DebugModule scheduler_module{"scheduler", "scheduler", depends_interrupt, {}, {}, 30, &start_sequence,
                                 &stop_sequence};
    DebugModule smp_module{"smp", "smp", depends_scheduler, {}, {}, 40, &start_sequence, &stop_sequence};
    DebugModule ipc_module{"ipc", "ipc", depends_scheduler, {}, {}, 50, &start_sequence, &stop_sequence};
    DebugModule syscall_module{"syscall", "syscall", depends_scheduler, {}, {}, 60, &start_sequence, &stop_sequence};
    DebugModule driver_module{"driver-core", "driver", depends_interrupt, {}, {}, 70, &start_sequence,
                              &stop_sequence};
    DebugModule vfs_module{"vfs", "filesystem", depends_driver, vfs_exports, {}, 80, &start_sequence, &stop_sequence};
    DebugModule posix_module{"posix", "posix", depends_vfs, {}, {}, 90, &start_sequence, &stop_sequence};
    DebugModule user_module{"user-mode", "user", depends_scheduler, {}, {}, 100, &start_sequence, &stop_sequence};
    DebugModule shell_module{"debug-shell", "shell", depends_vfs, {}, {}, 110, &start_sequence, &stop_sequence};

    ModuleManager manager;
    for (auto *module : {&arch_module, &memory_module, &interrupt_module, &scheduler_module, &smp_module, &ipc_module,
                         &syscall_module, &driver_module, &vfs_module, &posix_module, &user_module, &shell_module})
    {
        if (auto status = manager.register_module(*module); !status.ok())
        {
            return status;
        }
    }
    if (manager.register_module(arch_module).code() != StatusCode::already_exists)
    {
        return Status::fault("duplicate module name was not rejected");
    }
    if (auto status = manager.start_all(); !status.ok())
    {
        return status;
    }
    if (manager.failed_count() != 0 || manager.module_count() != 12 ||
        manager.services().query_raw("memory.frames") != &memory_module ||
        arch_module.started_at() >= memory_module.started_at())
    {
        return Status::fault("module manager dependency or service validation failed");
    }
    if (auto status = manager.stop_all(); !status.ok())
    {
        return status;
    }
    if (memory_module.stopped_at() >= arch_module.stopped_at())
    {
        return Status::fault("module manager stop order is not reverse dependency order");
    }

    constexpr std::array<ModuleDependency, 1> missing_dep{ModuleDependency{.name = "missing", .required = true}};
    DebugModule missing{"needs-missing", "test", missing_dep, {}, {}, 0};
    ModuleManager missing_manager;
    static_cast<void>(missing_manager.register_module(missing));
    if (missing_manager.start_all().code() != StatusCode::not_found)
    {
        return Status::fault("missing module dependency was not rejected");
    }

    constexpr std::array<ModuleDependency, 1> dep_b{ModuleDependency{.name = "cycle-b", .required = true}};
    constexpr std::array<ModuleDependency, 1> dep_a{ModuleDependency{.name = "cycle-a", .required = true}};
    DebugModule cycle_a{"cycle-a", "test", dep_b, {}, {}, 0};
    DebugModule cycle_b{"cycle-b", "test", dep_a, {}, {}, 0};
    ModuleManager cycle_manager;
    static_cast<void>(cycle_manager.register_module(cycle_a));
    static_cast<void>(cycle_manager.register_module(cycle_b));
    if (cycle_manager.start_all().code() != StatusCode::invalid_argument)
    {
        return Status::fault("module dependency cycle was not rejected");
    }

    report.modules = true;
    report.module_count = manager.module_count();
    report.module_failed_count = manager.failed_count();
    return Status::success();
}

Status run_vm_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    auto kernel_frame = kernel.memory().frames().allocate();
    auto user_frame = kernel.memory().frames().allocate();
    if (!kernel_frame || !user_frame)
    {
        return !kernel_frame ? kernel_frame.status() : user_frame.status();
    }

    memory::KernelAddressSpace kernel_space;
    memory::UserAddressSpace user_space;
    const auto kernel_test_address = test_mapping_address(kernel.arch().architecture());
    if (auto status = kernel_space.map_page(kernel_test_address, kernel_frame.value(), memory::page_read | memory::page_write);
        !status.ok())
    {
        return status;
    }
    if (auto status = kernel_space.unmap_page(kernel_test_address); !status.ok())
    {
        return status;
    }
    if (auto status = user_space.map_page(0x400000, user_frame.value(),
                                          memory::page_read | memory::page_write | memory::page_user);
        !status.ok())
    {
        return status;
    }
    if (user_space.valid(memory::UserSlice<const std::byte>{.address = 0, .count = 1}, memory::page_read) ||
        user_space.valid(memory::UserSlice<const std::byte>{.address = 0x500000, .count = 1}, memory::page_read))
    {
        return Status::fault("invalid user pointer was accepted");
    }
    constexpr std::string_view text{"hello"};
    std::array<std::byte, 6> source{};
    for (usize i = 0; i < text.size(); ++i)
    {
        source[i] = static_cast<std::byte>(text[i]);
    }
    source[text.size()] = std::byte{0};
    if (!user_space.copy_to_user(memory::UserSlice<std::byte>{.address = 0x400010, .count = source.size()}, source).ok())
    {
        return Status::fault("copy_to_user failed");
    }
    std::array<std::byte, 6> out{};
    if (!user_space.copy_from_user(memory::UserSlice<const std::byte>{.address = 0x400010, .count = out.size()}, out).ok())
    {
        return Status::fault("copy_from_user failed");
    }
    auto copied_string = user_space.copy_c_string_from_user(memory::UserPtr<const char>{.address = 0x400010}, 16);
    if (!copied_string || copied_string.value().view() != text)
    {
        return Status::fault("copy user string failed");
    }
    if (auto status = user_space.mark_copy_on_write(0x400000); !status.ok())
    {
        return status;
    }
    if (user_space.copy_to_user(memory::UserSlice<std::byte>{.address = 0x400010, .count = source.size()}, source)
            .status.code() != StatusCode::denied)
    {
        return Status::fault("write into read-only user mapping was not rejected");
    }
    if (memory::classify_page_fault(true, true, true, false, memory::page_read | memory::page_user) !=
        memory::PageFaultKind::write_to_read_only)
    {
        return Status::fault("page fault classification failed");
    }

    syscall::Table table;
    struct CopyContext
    {
        memory::UserAddressSpace *space;
    } context{.space = &user_space};
    if (auto status = table.register_callback(syscall::Number::ok_debug, "copy", &context,
                                              [](void *raw, const syscall::Request &request) {
                                                  auto *ctx = static_cast<CopyContext *>(raw);
                                                  std::array<std::byte, 6> buffer{};
                                                  auto copied = ctx->space->copy_from_user(
                                                      memory::UserSlice<const std::byte>{
                                                          .address = static_cast<uptr>(request.args[0]),
                                                          .count = static_cast<usize>(request.args[1]),
                                                      },
                                                      buffer);
                                                  return syscall::Response{
                                                      .value = copied.ok() ? static_cast<i64>(copied.bytes) : -1,
                                                      .status = copied.status,
                                                  };
                                              });
        !status.ok())
    {
        return status;
    }
    auto response = table.dispatch(syscall::Request{
        .number = syscall::Number::ok_debug,
        .args = {0x400010, 6, 0, 0, 0, 0},
    });
    if (!response.status.ok() || response.value != 6)
    {
        return Status::fault("safe user-copy syscall dispatch failed");
    }

    static_cast<void>(user_space.unmap_page(0x400000));
    static_cast<void>(kernel.memory().frames().release(kernel_frame.value()));
    static_cast<void>(kernel.memory().frames().release(user_frame.value()));
    report.vm = true;
    return Status::success();
}

Status run_process_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    auto elf = make_test_elf(kernel.arch().architecture());
    sched::ProcessManager processes;
    auto &ops = arch::arch_operations(kernel.arch().architecture());

    auto first = processes.create_kernel_thread("kthread-a", ops.make_kernel_context(0x1000, 0x8000));
    auto second = processes.create_kernel_thread("kthread-b", ops.make_kernel_context(0x2000, 0x9000));
    if (!first || !second || processes.process_count() != 2)
    {
        return Status::fault("kernel thread creation failed");
    }
    auto user = processes.create_user_process("hello", elf, kernel.arch().architecture());
    if (!user)
    {
        return user.status();
    }
    auto *user_process = processes.find(user.value());
    if (user_process == nullptr || user_process->memory_map().area_count() == 0 || user_process->threads().empty())
    {
        return Status::fault("user process ELF load failed");
    }
    auto child = processes.fork(user.value());
    if (!child)
    {
        return child.status();
    }
    const auto *child_process = processes.find(child.value());
    if (child_process == nullptr ||
        child_process->fd_table().open_count() != user_process->fd_table().open_count() ||
        child_process->memory_map().area_count() != user_process->memory_map().area_count())
    {
        return Status::fault("fork did not duplicate process metadata");
    }
    if (auto status = processes.execve(child.value(), elf, kernel.arch().architecture()); !status.ok())
    {
        return status;
    }
    if (auto status = processes.exit_group(child.value(), 17); !status.ok())
    {
        return status;
    }
    auto waited = processes.wait4(user.value(), child.value());
    if (!waited || waited.value() != 17 || processes.zombie_count() != 0)
    {
        return Status::fault("wait4 or zombie cleanup failed");
    }

    syscall::Request exit_request{.number = syscall::Number::exit, .caller = user.value(), .args = {0, 0, 0, 0, 0, 0}};
    auto exit_response = kernel.syscalls().dispatch(exit_request);
    if (!exit_response.status.ok())
    {
        return Status::fault("user process syscall exit path failed");
    }

    report.proc = true;
    report.elf = true;
    report.userland = true;
    return Status::success();
}

Status run_unix_vfs_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    fs::UnixVfsModel model;
    if (auto status = model.initialize(kernel.vfs()); !status.ok())
    {
        return status;
    }
    if (auto status = model.validate_mounts(); !status.ok())
    {
        return status;
    }
    if (auto status = model.validate_files(); !status.ok())
    {
        return status;
    }
    if (auto status = model.validate_directories(); !status.ok())
    {
        return status;
    }
    if (auto status = model.validate_symlink(); !status.ok())
    {
        return status;
    }
    if (auto status = model.validate_devices(); !status.ok())
    {
        return status;
    }
    if (auto status = model.validate_pipe(); !status.ok())
    {
        return status;
    }
    if (auto status = model.validate_tty(); !status.ok())
    {
        return status;
    }
    report.vfs_unix = true;
    report.devfs = true;
    report.pipe = true;
    report.tty = true;
    return Status::success();
}

Status run_linux_abi_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    syscall::LinuxSyscallAbi abi;
    syscall::LinuxSyscallFrame frame{
        .syscall_number = static_cast<u64>(syscall::Number::write),
        .rdi = 1,
        .rsi = 0x2000,
        .rdx = 5,
        .r10 = 4,
        .r8 = 3,
        .r9 = 2,
    };
    const auto request = abi.decode_x86_64(frame, 99);
    if (request.number != syscall::Number::write || request.args[0] != 1 || request.args[3] != 4 ||
        syscall::ErrnoMapper::result_for(syscall::Response{.value = -1, .status = Status::unsupported("unknown")}) !=
            -syscall::linux_ENOSYS)
    {
        return Status::fault("Linux syscall ABI decode or errno mapping failed");
    }
    syscall::LinuxSyscallDispatcher dispatcher{&kernel.syscalls()};
    syscall::LinuxSyscallFrame unknown{.syscall_number = 999999};
    if (dispatcher.dispatch_x86_64(unknown, kernel.scheduler().current_pid()) != -syscall::linux_ENOSYS)
    {
        return Status::fault("unknown Linux syscall did not return -ENOSYS");
    }
    constexpr std::string_view text{"linux"};
    syscall::LinuxSyscallFrame write_frame{
        .syscall_number = static_cast<u64>(syscall::Number::write),
        .rdi = 1,
        .rsi = reinterpret_cast<uptr>(text.data()),
        .rdx = text.size(),
    };
    if (dispatcher.dispatch_x86_64(write_frame, kernel.scheduler().current_pid()) != static_cast<i64>(text.size()))
    {
        return Status::fault("Linux write syscall smoke test failed");
    }
    syscall::LinuxCompatProcess process;
    process.set_tls_base(0x7000);
    if (process.tls_base() != 0x7000 || !process.auxv().add(3, 0x400000).ok())
    {
        return Status::fault("Linux TLS or auxv smoke test failed");
    }
    auto mapping = kernel.posix().mmap(0, 4096, posix::prot_READ | posix::prot_WRITE,
                                       posix::map_PRIVATE | posix::map_ANONYMOUS, -1, 0);
    if (!mapping)
    {
        return mapping.status();
    }
    if (auto status = kernel.posix().munmap(mapping.value(), 4096); !status.ok())
    {
        return status;
    }
    if (kernel.posix().futex(0x4000, posix::futex_WAKE, 1).status().ok() == false)
    {
        return Status::fault("Linux futex smoke test failed");
    }
    report.linux_abi = true;
    report.linux_syscalls = true;
    return Status::success();
}

Status run_driver_abi_roadmap_tests(KernelTestReport &report)
{
    driver::OkDriverRegistry registry;
    driver::NativeFakePciDriver native;
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
    if (!native.probed() || registry.bound_count() != 1 || registry.mmio().readl(0) != 0x0badc0de ||
        !registry.irq().registered)
    {
        return Status::fault("native driver ABI validation failed");
    }
    if (auto status = registry.remove_all(); !status.ok())
    {
        return status;
    }
    if (!native.removed())
    {
        return Status::fault("native driver remove validation failed");
    }

    driver::linux_compat::LinuxPciShim shim;
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
        .probe = [](driver::OkDevice &, const driver::linux_compat::pci_device_id &) {
            shim_state.probed = true;
            return Status::success();
        },
        .remove = [](driver::OkDevice &) {
            shim_state.removed = true;
            return Status::success();
        },
    };
    driver::OkDevice device{.bus = driver::OkBusType::pci,
                            .id = driver::OkDeviceId{.vendor = 0x1af4, .device = 0x1000, .class_code = 0x02}};
    if (auto status = shim.register_driver(linux_driver); !status.ok())
    {
        return status;
    }
    if (auto status = shim.bind(device); !status.ok())
    {
        return status;
    }
    auto *mmio = shim.ioremap(0x1000, 64);
    if (mmio == nullptr || !mmio->writel(0, 0xfeed).ok() || mmio->readl(0) != 0xfeed)
    {
        return Status::fault("Linux driver shim MMIO validation failed");
    }
    auto *allocation = shim.kmalloc(16);
    if (allocation == nullptr || !shim.kfree(allocation).ok())
    {
        return Status::fault("Linux driver shim allocation validation failed");
    }
    driver::OkSpinLock lock;
    if (!lock.try_lock())
    {
        return Status::fault("Linux driver shim spinlock validation failed");
    }
    lock.unlock();
    driver::OkIrqHandle irq{};
    if (!shim.request_irq(irq, 44).ok() || !shim.free_irq(irq).ok())
    {
        return Status::fault("Linux driver shim IRQ validation failed");
    }
    if (auto status = shim.remove(device); !status.ok())
    {
        return status;
    }
    if (!shim_state.probed || !shim_state.removed)
    {
        return Status::fault("Linux driver shim probe/remove validation failed");
    }
    report.driver_abi = true;
    report.linux_driver_shim = true;
    report.module_load = true;
    return Status::success();
}

Status run_network_storage_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    net::VirtioNetDevice netdev;
    if (auto status = netdev.probe(); !status.ok())
    {
        return status;
    }
    if (auto status = netdev.start(net::EthernetAddress{{0x02, 0, 0, 0, 0, 2}}); !status.ok())
    {
        return status;
    }
    std::array<std::byte, 64> frame{};
    if (auto status = netdev.transmit(frame); !status.ok())
    {
        return status;
    }
    if (auto status = netdev.receive(frame); !status.ok())
    {
        return status;
    }
    net::ArpCache arp;
    if (auto status = arp.learn(kernel.network().local_address(), netdev.mac()); !status.ok())
    {
        return status;
    }
    if (!arp.lookup(kernel.network().local_address()))
    {
        return Status::fault("ARP cache validation failed");
    }

    net::SocketTable sockets;
    if (auto status = sockets.initialize(kernel.network()); !status.ok())
    {
        return status;
    }
    auto udp = sockets.socket(net::SocketType::udp);
    if (!udp)
    {
        return udp.status();
    }
    if (auto status = sockets.bind(udp.value(), net::UdpEndpoint{.address = kernel.network().local_address(), .port = 41000});
        !status.ok())
    {
        return status;
    }
    constexpr std::string_view payload{"sock"};
    auto sent = sockets.sendto(udp.value(),
                               std::span<const std::byte>{reinterpret_cast<const std::byte *>(payload.data()),
                                                           payload.size()},
                               net::UdpEndpoint{.address = kernel.network().local_address(), .port = 41001});
    if (!sent || sent.value() != payload.size())
    {
        return Status::fault("UDP socket send validation failed");
    }
    auto received = sockets.recvfrom(udp.value());
    if (!received || received.value().payload_size != payload.size())
    {
        return Status::fault("UDP socket receive validation failed");
    }
    auto tcp_listener = sockets.socket(net::SocketType::tcp);
    auto tcp_client = sockets.socket(net::SocketType::tcp);
    if (!tcp_listener || !tcp_client)
    {
        return Status::fault("TCP socket allocation failed");
    }
    if (auto status =
            sockets.bind(tcp_listener.value(), net::UdpEndpoint{.address = kernel.network().local_address(), .port = 42000});
        !status.ok())
    {
        return status;
    }
    if (auto status = sockets.listen(tcp_listener.value()); !status.ok())
    {
        return status;
    }
    if (auto status = sockets.connect(tcp_client.value(),
                                      net::UdpEndpoint{.address = kernel.network().local_address(), .port = 42000});
        !status.ok())
    {
        return status;
    }
    if (!sockets.accept(tcp_listener.value()))
    {
        return Status::fault("TCP accept validation failed");
    }

    fs::BlockCache cache;
    if (auto status = cache.attach(kernel.disk()); !status.ok())
    {
        return status;
    }
    std::array<std::byte, driver::block_sector_size> block{};
    block[0] = std::byte{0x5a};
    if (auto status = cache.write_block(4, block); !status.ok())
    {
        return status;
    }
    std::array<std::byte, driver::block_sector_size> read_block{};
    if (auto status = cache.read_block(4, read_block); !status.ok())
    {
        return status;
    }
    if (auto status = cache.read_block(4, read_block); !status.ok())
    {
        return status;
    }
    if (cache.stats().misses == 0 || cache.stats().hits == 0 || read_block[0] != std::byte{0x5a})
    {
        return Status::fault("block cache validation failed");
    }
    if (cache.read_block(kernel.disk().geometry().block_count, read_block).code() != StatusCode::invalid_argument)
    {
        return Status::fault("out-of-range block read was not rejected");
    }
    std::array<std::byte, driver::block_sector_size> mbr{};
    mbr[446 + 4] = std::byte{0x83};
    write_le32(mbr, 446 + 8, 2048);
    write_le32(mbr, 446 + 12, 128);
    mbr[510] = std::byte{0x55};
    mbr[511] = std::byte{0xaa};
    fs::PartitionTable partitions;
    if (auto status = partitions.parse_mbr(mbr); !status.ok())
    {
        return status;
    }
    if (partitions.partition_count() != 1 || partitions.partition(0)->first_lba != 2048)
    {
        return Status::fault("partition table validation failed");
    }

    report.netdev = true;
    report.sockets = true;
    report.block = true;
    report.ext4_readonly = true;
    return Status::success();
}

Status run_smp_irq_preempt_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    smp::CpuTopology topology;
    if (auto status = topology.initialize(4); !status.ok())
    {
        return status;
    }
    for (smp::CpuId cpu = 0; cpu < 4; ++cpu)
    {
        if (cpu != 0)
        {
            if (auto status = topology.mark_starting(cpu); !status.ok())
            {
                return status;
            }
        }
        if (auto status = topology.mark_online(cpu); !status.ok())
        {
            return status;
        }
        if (auto status = topology.record_schedule(cpu); !status.ok())
        {
            return status;
        }
    }
    if (topology.online_count() != 4)
    {
        return Status::fault("SMP online validation failed");
    }
    smp::PerCpu<uptr> stacks;
    for (smp::CpuId cpu = 0; cpu < 4; ++cpu)
    {
        stacks.get(cpu) = 0x800000 + static_cast<uptr>(cpu) * 0x4000;
    }
    if (stacks.get(0) == stacks.get(1))
    {
        return Status::fault("per-CPU stack validation failed");
    }

    interrupt::SimulatedInterruptController controller;
    if (auto status = controller.initialize(); !status.ok())
    {
        return status;
    }
    if (auto status = controller.enable(32); !status.ok())
    {
        return status;
    }
    arch::TrapFrame timer{.vector = 32};
    if (auto status = kernel.interrupts().dispatch(timer); !status.ok())
    {
        return status;
    }
    if (!controller.enabled(32) || kernel.interrupts().handled_count(32) == 0 ||
        !kernel.interrupts().has_handler(32))
    {
        return Status::fault("interrupt controller validation failed");
    }

    sched::Scheduler scheduler;
    if (auto status = scheduler.configure_cpus(1); !status.ok())
    {
        return status;
    }
    auto &ops = arch::arch_operations(kernel.arch().architecture());
    auto first = scheduler.create_process("preempt-a", ops.make_kernel_context(0x1000, 0x8000));
    auto second = scheduler.create_process("preempt-b", ops.make_kernel_context(0x2000, 0x9000));
    if (!first || !second)
    {
        return Status::fault("preemption scheduler setup failed");
    }
    static_cast<void>(scheduler.set_runnable(first.value()));
    static_cast<void>(scheduler.set_runnable(second.value()));
    smp::PreemptionController preempt;
    if (auto status = preempt.initialize(1); !status.ok())
    {
        return status;
    }
    if (auto status = preempt.tick(0, scheduler); !status.ok())
    {
        return status;
    }
    preempt.disable(0);
    if (auto status = preempt.tick(0, scheduler); !status.ok())
    {
        return status;
    }
    if (auto status = preempt.enable(0); !status.ok())
    {
        return status;
    }
    if (preempt.ticks(0) < 2 || preempt.switches(0) != 1)
    {
        return Status::fault("preemption tick/switch validation failed");
    }
    if (auto status = preempt.sleep_current(0, 2); !status.ok())
    {
        return status;
    }
    if (preempt.preemptible(0))
    {
        return Status::fault("sleeping CPU was preemptible");
    }
    if (auto status = preempt.wake_sleepers(preempt.ticks(0) + 2); !status.ok())
    {
        return status;
    }

    report.smp_roadmap = true;
    report.irq_roadmap = true;
    report.preempt = true;
    return Status::success();
}

} // namespace

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

    const auto geometry = disk().geometry();
    if (geometry.block_count == 0 || geometry.block_size != driver::block_sector_size || !geometry.writable)
    {
        return Status::fault("block driver debug test failed");
    }
    static_cast<void>(simplefs_.unlink("/suite.txt"));
    if (auto status = simplefs_.create("/suite.txt", fs::NodeType::regular); !status.ok())
    {
        return status;
    }
    constexpr std::string_view simplefs_text{"simplefs"};
    if (auto status = simplefs_.write_file("/suite.txt", as_bytes(simplefs_text)); !status.ok())
    {
        return status;
    }
    auto simplefs_file = simplefs_.read_file("/suite.txt");
    if (!simplefs_file || simplefs_file.value().size != simplefs_text.size())
    {
        return Status::fault("SimpleFS read debug test failed");
    }
    auto simplefs_stat = simplefs_.stat("/suite.txt");
    if (!simplefs_stat || simplefs_stat.value().size != simplefs_text.size())
    {
        return Status::fault("SimpleFS stat debug test failed");
    }
    auto simplefs_listing = simplefs_.list_root();
    if (!simplefs_listing || simplefs_listing.value().count == 0)
    {
        return Status::fault("SimpleFS list debug test failed");
    }
    test_report_.simplefs = true;

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
    if (!syscalls_.has_handler(syscall::Number::openat) || !syscalls_.has_handler(syscall::Number::newfstatat) ||
        !syscalls_.has_handler(syscall::Number::mmap) || !syscalls_.has_handler(syscall::Number::arch_prctl))
    {
        return Status::fault("glibc baseline syscall handlers are missing");
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
    if (!virtio_gpu_driver_.bound() || virtio_gpu_driver_.frames_presented() == 0)
    {
        return Status::fault("virtio GPU display debug test failed");
    }
    test_report_.gpu = true;

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

    if (pci_bus_driver_.device_count() == 0 || pci_bus_driver_.find_class(0x0c, 0x03, 0x30) == nullptr ||
        pci_bus_driver_.find_class(0x01, 0x00, 0x00) == nullptr)
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

    constexpr std::string_view udp_payload{"netdbg"};
    if (auto status = network_.send_udp(net::UdpEndpoint{.address = network_.local_address(), .port = 30000},
                                        net::UdpEndpoint{.address = network_.local_address(), .port = 30001},
                                        as_bytes(udp_payload));
        !status.ok())
    {
        return status;
    }
    auto datagram = network_.receive_udp();
    if (!datagram || datagram.value().payload_size != udp_payload.size())
    {
        return Status::fault("UDP/IP debug test failed");
    }
    if (auto status = network_.listen_tcp(31337); !status.ok())
    {
        return status;
    }
    auto tcp = network_.connect_tcp(net::UdpEndpoint{.address = network_.local_address(), .port = 31337}, 31338);
    if (!tcp || tcp.value().state != net::TcpState::established)
    {
        return Status::fault("TCP debug test failed");
    }
    test_report_.net = true;

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
    auto posix_stat = posix_.fstat(fd.value());
    if (!posix_stat || posix_stat.value().size != posix_text.size())
    {
        return Status::fault("POSIX fstat debug test failed");
    }
    if (auto status = posix_.close(fd.value()); !status.ok())
    {
        return status;
    }
    if (auto status = posix_.chdir("/tmp"); !status.ok())
    {
        return status;
    }
    auto relative_fd = posix_.openat(posix::at_FDCWD, "posix-relative.txt",
                                     posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC);
    if (!relative_fd)
    {
        return relative_fd.status();
    }
    if (auto status = posix_.close(relative_fd.value()); !status.ok())
    {
        return status;
    }
    if (auto relative_stat = posix_.statat(posix::at_FDCWD, "posix-relative.txt"); !relative_stat)
    {
        return Status::fault("POSIX relative path debug test failed");
    }
    auto dir_fd = posix_.open("/", posix::o_RDONLY | posix::o_DIRECTORY);
    if (!dir_fd)
    {
        return dir_fd.status();
    }
    std::array<std::byte, 256> dirents{};
    auto dirent_bytes = posix_.getdents64(dir_fd.value(), dirents);
    if (!dirent_bytes || dirent_bytes.value() == 0)
    {
        return Status::fault("POSIX getdents64 debug test failed");
    }
    if (auto status = posix_.close(dir_fd.value()); !status.ok())
    {
        return status;
    }
    auto mapping = posix_.mmap(0, 4096, posix::prot_READ | posix::prot_WRITE,
                               posix::map_PRIVATE | posix::map_ANONYMOUS, -1, 0);
    if (!mapping)
    {
        return mapping.status();
    }
    if (auto status = posix_.mprotect(mapping.value(), 4096, posix::prot_READ); !status.ok())
    {
        return status;
    }
    if (auto status = posix_.munmap(mapping.value(), 4096); !status.ok())
    {
        return status;
    }
    if (auto status = posix_.chdir("/"); !status.ok())
    {
        return status;
    }
    test_report_.posix = true;

    auto shell_status = debug_shell_.execute("status");
    if (!shell_status || shell_status.value().empty())
    {
        return Status::fault("debug shell status test failed");
    }
    auto shell_posix = debug_shell_.execute("posix");
    if (!shell_posix || shell_posix.value().empty())
    {
        return Status::fault("debug shell POSIX test failed");
    }
    auto shell_ls = debug_shell_.execute("ls /tmp");
    if (!shell_ls || shell_ls.value().empty())
    {
        return Status::fault("debug shell ls test failed");
    }
    auto shell_user = debug_shell_.execute("whoami");
    if (!shell_user || shell_user.value().empty())
    {
        return Status::fault("debug shell user test failed");
    }
    auto shell_su = debug_shell_.execute("su root");
    if (!shell_su || shell_su.value().empty())
    {
        return Status::fault("debug shell su test failed");
    }
    auto shell_sfs_write = debug_shell_.execute("sfs write shell.txt hello");
    if (!shell_sfs_write)
    {
        return Status::fault("debug shell SimpleFS write test failed");
    }
    auto shell_sfs_ls = debug_shell_.execute("sfs ls");
    if (!shell_sfs_ls || shell_sfs_ls.value().empty())
    {
        return Status::fault("debug shell SimpleFS ls test failed");
    }
    auto shell_sh = debug_shell_.execute("false && echo bad || echo ok # comment");
    if (!shell_sh || shell_sh.value() != "ok\n")
    {
        return Status::fault("debug shell sh operator test failed");
    }
    auto shell_net_send = debug_shell_.execute("net udp 30002 shell-net");
    if (!shell_net_send)
    {
        return Status::fault("debug shell network send test failed");
    }
    auto shell_net_recv = debug_shell_.execute("net recv");
    if (!shell_net_recv || shell_net_recv.value().empty())
    {
        return Status::fault("debug shell network receive test failed");
    }
    auto shell_ext4 = debug_shell_.execute("ext4 status");
    if (!shell_ext4 || shell_ext4.value().empty())
    {
        return Status::fault("debug shell ext4 status test failed");
    }
    auto shell_su_kernel = debug_shell_.execute("su kernel");
    if (!shell_su_kernel || shell_su_kernel.value().empty())
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

    if (auto status = run_module_roadmap_tests(test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_vm_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_process_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_unix_vfs_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_linux_abi_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_driver_abi_roadmap_tests(test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_network_storage_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }
    if (auto status = run_smp_irq_preempt_roadmap_tests(*this, test_report_); !status.ok())
    {
        return status;
    }

    auto debug_test_points = test::run_kernel_test_points(*this);
    if (!debug_test_points)
    {
        return debug_test_points.status();
    }
    debug_test_points_run_ = debug_test_points.value();

    return Status::success();
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
    if (auto status = volume.read_block(1, block); !status.ok())
    {
        return status;
    }

    auto &block_device = disk();
    if (auto status = block_device.write_blocks(0, std::span<const std::byte>(image.data(), image.size()));
        !status.ok())
    {
        return status;
    }
    fs::Ext4Volume block_volume;
    if (auto status = block_volume.mount(block_device); !status.ok())
    {
        return status;
    }
    if (auto status = block_volume.read_block(1, block); !status.ok())
    {
        return status;
    }
    return simplefs_.format(block_device, "okroot");
}

} // namespace ok
