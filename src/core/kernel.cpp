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

std::string_view user_label_for(const user::UserSpaceManager &users, user::Credentials credentials)
{
    if (credentials.kernel_space)
    {
        return "kernel";
    }
    if (const auto *account = users.users().find_by_uid(credentials.euid); account != nullptr)
    {
        return account->name.view();
    }
    return "user";
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
    gui_mouse_left_down_ = false;
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

Status Kernel::handle_gui_mouse(i32 delta_x, i32 delta_y, bool left_button)
{
    if (!booted_)
    {
        return Status::not_initialized("kernel is not booted");
    }
    auto &compositor = gui_module_.compositor();
    if (compositor.state() != gui::GuiState::running)
    {
        return Status::not_initialized("GUI compositor is not running");
    }

    const bool click = left_button && !gui_mouse_left_down_;
    const auto shell_surface = debug_shell_.gui_surface_id();
    const auto file_manager_surface = file_manager_.surface_id();
    const auto file_manager_process = file_manager_.process_id();
    if (auto status = compositor.handle_mouse_delta(delta_x, delta_y, left_button); !status.ok())
    {
        return status;
    }
    if (shell_surface != 0 && !compositor.surface_info(shell_surface))
    {
        debug_shell_.mark_gui_closed();
    }
    if (file_manager_surface != 0 && !compositor.surface_info(file_manager_surface))
    {
        if (file_manager_process != 0)
        {
            static_cast<void>(scheduler_.kill_process(file_manager_process));
        }
        file_manager_.mark_closed();
    }
    if (click)
    {
        if (auto status =
                file_manager_.handle_mouse(compositor, vfs_, compositor.pointer_x(), compositor.pointer_y(), true);
            !status.ok())
        {
            return status;
        }
    }
    gui_mouse_left_down_ = left_button;
    return Status::success();
}

Status Kernel::open_file_manager(std::string_view path)
{
    if (!booted_)
    {
        return Status::not_initialized("kernel is not booted");
    }
    const auto credentials = posix_.user_credentials();
    FixedString<sched::max_process_name> process_name;
    if (auto status = process_name.assign("fm:"); !status.ok())
    {
        return status;
    }
    if (auto status = process_name.append(user_label_for(user_space_, credentials)); !status.ok())
    {
        return status;
    }

    const auto process_offset = scheduler_.process_count() * 0x1000;
    const auto context = arch_->make_kernel_context(0x7000 + process_offset, 0xe000 + process_offset);
    auto process = scheduler_.create_background_process(process_name.view(), context);
    if (!process)
    {
        return process.status();
    }
    if (auto status = scheduler_.set_credentials(process.value(), credentials); !status.ok())
    {
        static_cast<void>(scheduler_.kill_process(process.value()));
        return status;
    }

    const auto previous_process = file_manager_.process_id();
    if (auto status = file_manager_.open(gui_module_.compositor(), vfs_, path, credentials, process.value());
        !status.ok())
    {
        static_cast<void>(scheduler_.kill_process(process.value()));
        return status;
    }
    if (previous_process != 0 && previous_process != process.value())
    {
        static_cast<void>(scheduler_.kill_process(previous_process));
    }
    return Status::success();
}

Status Kernel::close_file_manager()
{
    const auto process = file_manager_.process_id();
    if (auto status = file_manager_.close(gui_module_.compositor()); !status.ok())
    {
        return status;
    }
    if (process != 0)
    {
        static_cast<void>(scheduler_.kill_process(process));
    }
    return Status::success();
}

Status Kernel::kill_process(sched::ProcessId pid)
{
    if (pid == 0)
    {
        return Status::invalid_argument("process id must be non-zero");
    }
    if (auto status = scheduler_.kill_process(pid); !status.ok())
    {
        return status;
    }

    if (file_manager_.process_id() == pid)
    {
        auto &compositor = gui_module_.compositor();
        const auto surface = file_manager_.surface_id();
        if (compositor.state() == gui::GuiState::running && surface != 0 && compositor.surface_info(surface))
        {
            if (auto status = file_manager_.close(compositor); !status.ok())
            {
                file_manager_.mark_closed();
                return status;
            }
        }
        else
        {
            file_manager_.mark_closed();
        }
    }
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
