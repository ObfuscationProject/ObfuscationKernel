#include "ok/core/kernel.hpp"

namespace ok
{
namespace
{

std::span<const memory::MemoryRegion> memory_map_span(const KernelConfig &config)
{
    return {config.memory_map.data(), config.memory_region_count};
}

driver::DisplayBackend display_backend_for_arch(arch::Architecture architecture)
{
    switch (architecture)
    {
    case arch::Architecture::i386:
    case arch::Architecture::x86_64:
        return driver::DisplayBackend::virtio_gpu_pci;
    case arch::Architecture::aarch64:
    case arch::Architecture::rv64:
        return driver::DisplayBackend::ramfb;
    case arch::Architecture::arm32:
    case arch::Architecture::rv32:
    case arch::Architecture::loongarch64:
    case arch::Architecture::mips:
    case arch::Architecture::mips64:
    case arch::Architecture::ppc:
        return driver::DisplayBackend::memory_framebuffer;
    }
    return driver::DisplayBackend::memory_framebuffer;
}

} // namespace

Kernel::Kernel() : arch_(&arch::arch_operations(arch::configured_architecture()))
{
}

Status Kernel::boot(KernelConfig config)
{
    config.architecture = arch::configured_architecture();
    test_report_ = {};
    kernel_modules_ = {};
    static_cast<void>(gui_module_.stop());
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
    display_driver_.set_backend(display_backend_for_arch(config_.architecture));
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
    if (auto status = drivers_.add(ram_block_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(virtio_block_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(display_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = drivers_.add(virtio_gpu_driver_); !status.ok())
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
    if (const auto *block = pci_bus_driver_.find_class(0x01, 0x00, 0x00); block != nullptr)
    {
        if (auto status = virtio_block_driver_.bind(*block); !status.ok())
        {
            return status;
        }
    }
    if (const auto *gpu = pci_bus_driver_.find_class(0x03, 0x00, 0x00); gpu != nullptr)
    {
        if (auto status = virtio_gpu_driver_.bind(*gpu); !status.ok())
        {
            return status;
        }
        display_driver_.set_backend(driver::DisplayBackend::virtio_gpu_pci);
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

    if (auto status = drivers_.bind_kernel_processes(scheduler_, *arch_, 0x10000, 0x20000); !status.ok())
    {
        return status;
    }

    if (auto status = kernel_modules_.bind_kernel_process(scheduler_, *arch_, 0x2000, 0x9000); !status.ok())
    {
        return status;
    }
    gui_module_.bind_display(display_driver_);
    if (auto status = kernel_modules_.register_module(gui_module_); !status.ok())
    {
        return status;
    }
    if (auto status = kernel_modules_.start_all(); !status.ok())
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
    if (auto status = gui_module_.compositor().play_startup_animation(); !status.ok())
    {
        return status;
    }
    if (virtio_gpu_driver_.bound())
    {
        if (auto status = virtio_gpu_driver_.present(display_driver_); !status.ok())
        {
            return status;
        }
    }
    if (auto status = register_builtin_interrupts(timer_driver_); !status.ok())
    {
        return status;
    }
    if (auto status = posix_.initialize(vfs_, console_driver_, scheduler_); !status.ok())
    {
        return status;
    }
    if (auto status = network_.initialize(net::Ipv4Address{{127, 0, 0, 1}}); !status.ok())
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

    if (auto status = vfs_.create("/tmp/kernel.log", fs::NodeType::regular); !status.ok())
    {
        return status;
    }
    if (auto status = log_boot_line("[    0.000005] fs: ram vfs mounted at /"); !status.ok())
    {
        return status;
    }
    if (auto status = simplefs_.format(disk(), "okroot"); !status.ok())
    {
        return status;
    }
    if (auto status = log_boot_line("[    0.000006] fs: simplefs formatted on block device"); !status.ok())
    {
        return status;
    }

    booted_ = true;
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

Status Kernel::register_builtin_interrupts(driver::TimerDriver &timer)
{
    return interrupts_.register_callback(32, "timer", &timer, [](void *context, arch::TrapFrame &) {
        static_cast<driver::TimerDriver *>(context)->tick();
        return Status::success();
    });
}

} // namespace ok
