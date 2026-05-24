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

std::span<const std::byte> bytes_for(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

template <usize Capacity> Status append_unsigned(FixedString<Capacity> &out, u64 value)
{
    constexpr u64 powers[] = {
        10'000'000'000'000'000'000ull,
        1'000'000'000'000'000'000ull,
        100'000'000'000'000'000ull,
        10'000'000'000'000'000ull,
        1'000'000'000'000'000ull,
        100'000'000'000'000ull,
        10'000'000'000'000ull,
        1'000'000'000'000ull,
        100'000'000'000ull,
        10'000'000'000ull,
        1'000'000'000ull,
        100'000'000ull,
        10'000'000ull,
        1'000'000ull,
        100'000ull,
        10'000ull,
        1'000ull,
        100ull,
        10ull,
        1ull,
    };
    bool started = false;
    for (const auto power : powers)
    {
        u8 digit = 0;
        while (value >= power)
        {
            value -= power;
            ++digit;
        }
        if (digit != 0 || started || power == 1)
        {
            if (auto status = out.append(static_cast<char>('0' + digit)); !status.ok())
            {
                return status;
            }
            started = true;
        }
    }
    return Status::success();
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
    if (auto status = ensure_kernel_log_file(); !status.ok())
    {
        return status;
    }

    if (auto status = drivers_.bind_kernel_processes(scheduler_, *arch_, 0x10000, 0x20000); !status.ok())
    {
        return status;
    }
    if (auto status = log_boot_line("[ daemon ] driver: guard armed for built-in driver processes"); !status.ok())
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
    if (auto status = log_boot_line("[ daemon ] module: guard armed for kernel module processes"); !status.ok())
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

    if (auto status = ensure_kernel_log_file(); !status.ok())
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
    const auto file_manager_surface = file_manager_.surface_id();
    const auto file_manager_process = file_manager_.process_id();
    if (auto status = compositor.handle_mouse_delta(delta_x, delta_y, left_button); !status.ok())
    {
        return status;
    }
    const auto window_event = compositor.consume_window_event();
    if (window_event.kind != gui::WindowEventKind::none)
    {
        if (auto status = handle_gui_window_event(window_event); !status.ok())
        {
            return status;
        }
    }
    if (auto status = debug_shell_.reconcile_gui_windows(); !status.ok())
    {
        return status;
    }
    if (file_manager_surface != 0 && !compositor.surface_info(file_manager_surface))
    {
        if (file_manager_process != 0)
        {
            static_cast<void>(scheduler_.kill_process(file_manager_process));
            debug_shell_.notify_process_exit(file_manager_process);
        }
        file_manager_.mark_closed();
    }
    if (click && window_event.kind == gui::WindowEventKind::none)
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

Status Kernel::handle_gui_window_event(gui::WindowEvent event)
{
    switch (event.kind)
    {
    case gui::WindowEventKind::none:
        return Status::success();
    case gui::WindowEventKind::close_request:
        return handle_gui_close_request(event.surface_id);
    case gui::WindowEventKind::resized:
    case gui::WindowEventKind::maximized:
    case gui::WindowEventKind::restored:
        return handle_gui_surface_changed(event.surface_id);
    case gui::WindowEventKind::minimized:
        clear_gui_close_attempt(event.surface_id);
        if (debug_shell_.owns_surface(event.surface_id))
        {
            return debug_shell_.handle_surface_changed(event.surface_id);
        }
        if (file_manager_.surface_id() == event.surface_id)
        {
            return file_manager_.handle_surface_changed(gui_module_.compositor(), vfs_);
        }
        return Status::success();
    }
    return Status::success();
}

Status Kernel::handle_gui_close_request(gui::SurfaceId surface)
{
    auto &compositor = gui_module_.compositor();
    if (!compositor.surface_info(surface))
    {
        clear_gui_close_attempt(surface);
        return Status::success();
    }

    Status status = Status::success();
    if (debug_shell_.owns_surface(surface))
    {
        status = debug_shell_.close_surface_window(surface);
    }
    else if (file_manager_.surface_id() == surface)
    {
        status = close_file_manager();
    }
    else
    {
        status = compositor.close_surface(surface);
        if (status.ok())
        {
            static_cast<void>(compositor.present());
        }
    }
    if (!status.ok() && status.code() != StatusCode::not_found)
    {
        return status;
    }
    if (!compositor.surface_info(surface))
    {
        clear_gui_close_attempt(surface);
        return Status::success();
    }
    return note_ignored_gui_close(surface);
}

Status Kernel::handle_gui_surface_changed(gui::SurfaceId surface)
{
    clear_gui_close_attempt(surface);
    if (debug_shell_.owns_surface(surface))
    {
        return debug_shell_.handle_surface_changed(surface);
    }
    if (file_manager_.surface_id() == surface)
    {
        return file_manager_.handle_surface_changed(gui_module_.compositor(), vfs_);
    }
    return gui_module_.compositor().present();
}

Status Kernel::force_close_gui_surface(gui::SurfaceId surface)
{
    if (debug_shell_.owns_surface(surface))
    {
        return debug_shell_.close_surface_window(surface);
    }
    if (file_manager_.surface_id() == surface)
    {
        return close_file_manager();
    }
    auto &compositor = gui_module_.compositor();
    if (compositor.surface_info(surface))
    {
        if (auto status = compositor.close_surface(surface); !status.ok())
        {
            return status;
        }
        return compositor.present();
    }
    return Status::success();
}

Status Kernel::note_ignored_gui_close(gui::SurfaceId surface)
{
    for (auto &attempt : gui_close_attempts_)
    {
        if (attempt.surface == surface)
        {
            ++attempt.count;
            if (attempt.count >= 3)
            {
                clear_gui_close_attempt(surface);
                return force_close_gui_surface(surface);
            }
            if (attempt.count == 2 && attempt.prompt_surface == 0)
            {
                return show_force_close_prompt(surface);
            }
            return Status::success();
        }
    }
    return gui_close_attempts_.push_back(GuiCloseAttempt{.surface = surface, .prompt_surface = 0, .count = 1});
}

Status Kernel::show_force_close_prompt(gui::SurfaceId surface)
{
    auto &compositor = gui_module_.compositor();
    auto desktop = compositor.desktop_bounds();
    if (!desktop)
    {
        return desktop.status();
    }
    const gui::Rect bounds{.x = static_cast<i32>(desktop.value().width > 260 ? (desktop.value().width - 260) / 2 : 8),
                           .y = static_cast<i32>(desktop.value().height > 70 ? (desktop.value().height - 70) / 2 : 8),
                           .width = desktop.value().width > 260 ? 260u : desktop.value().width,
                           .height = desktop.value().height > 70 ? 70u : desktop.value().height};
    auto prompt = compositor.create_surface(bounds, "force-close");
    if (!prompt)
    {
        return prompt.status();
    }
    if (auto status = compositor.fill(prompt.value(), 0xff201018u); !status.ok())
    {
        return status;
    }
    if (auto status = compositor.draw_text(prompt.value(), 2, 2, "close ignored", 0xffffcc66u, 0xff201018u);
        !status.ok())
    {
        return status;
    }
    if (auto status =
            compositor.draw_text(prompt.value(), 2, 4, "click close again to force", 0xffd8f3ffu, 0xff201018u);
        !status.ok())
    {
        return status;
    }
    for (auto &attempt : gui_close_attempts_)
    {
        if (attempt.surface == surface)
        {
            attempt.prompt_surface = prompt.value();
            break;
        }
    }
    return compositor.present();
}

void Kernel::clear_gui_close_attempt(gui::SurfaceId surface)
{
    auto &compositor = gui_module_.compositor();
    for (usize i = 0; i < gui_close_attempts_.size();)
    {
        if (gui_close_attempts_[i].surface != surface && gui_close_attempts_[i].prompt_surface != surface)
        {
            ++i;
            continue;
        }
        const auto prompt = gui_close_attempts_[i].prompt_surface;
        if (prompt != 0 && prompt != surface && compositor.state() == gui::GuiState::running &&
            compositor.surface_info(prompt))
        {
            static_cast<void>(compositor.destroy_surface(prompt));
        }
        static_cast<void>(gui_close_attempts_.erase_at(i));
    }
}

Status Kernel::open_file_manager(std::string_view path, bool foreground_shell_child)
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
    auto process = scheduler_.create_process(process_name.view(), context);
    if (!process)
    {
        return process.status();
    }
    if (auto status = scheduler_.set_runnable(process.value()); !status.ok())
    {
        static_cast<void>(scheduler_.kill_process(process.value()));
        return status;
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
        debug_shell_.notify_process_exit(previous_process);
    }
    if (foreground_shell_child)
    {
        if (auto status = debug_shell_.start_foreground_process(process.value()); !status.ok())
        {
            return status;
        }
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
        debug_shell_.notify_process_exit(process);
    }
    return Status::success();
}

Status Kernel::kill_process(sched::ProcessId pid)
{
    if (pid == 0)
    {
        return Status::invalid_argument("process id must be non-zero");
    }
    const bool shell_process = debug_shell_.owns_process(pid);
    const bool file_manager_process = file_manager_.process_id() == pid;
    if (auto status = scheduler_.kill_process(pid); !status.ok())
    {
        return status;
    }

    if (shell_process)
    {
        if (auto status = debug_shell_.close_process_window(pid); !status.ok() && status.code() != StatusCode::not_found)
        {
            return status;
        }
    }
    debug_shell_.notify_process_exit(pid);
    if (file_manager_process)
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
    return supervise_daemons();
}

Status Kernel::supervise_daemons()
{
    StaticVector<driver::DriverManager::DriverProcessRestart, driver::max_drivers> driver_restarts;
    if (auto status = drivers_.supervise_kernel_processes(driver_restarts); !status.ok())
    {
        return status;
    }
    for (const auto &restart : driver_restarts)
    {
        if (auto status = log_daemon_restart("driver", restart.process_name.view(), restart.previous_pid, restart.pid);
            !status.ok())
        {
            return status;
        }
    }

    StaticVector<ModuleManager::ModuleProcessRestart, max_kernel_modules> module_restarts;
    if (auto status = kernel_modules_.supervise_kernel_processes(module_restarts); !status.ok())
    {
        return status;
    }
    for (const auto &restart : module_restarts)
    {
        if (auto status = log_daemon_restart("module", restart.process_name.view(), restart.previous_pid, restart.pid);
            !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status Kernel::ensure_kernel_log_file()
{
    auto stat = vfs_.stat("/tmp/kernel.log");
    if (stat)
    {
        return Status::success();
    }
    if (stat.status().code() != StatusCode::not_found)
    {
        return stat.status();
    }
    return vfs_.create("/tmp/kernel.log", fs::NodeType::regular);
}

Status Kernel::append_kernel_log_line(std::string_view line)
{
    auto file = vfs_.read_file("/tmp/kernel.log");
    if (!file)
    {
        if (file.status().code() == StatusCode::not_found)
        {
            return Status::success();
        }
        return file.status();
    }

    FixedString<fs::max_file_data + 1> buffer;
    for (usize i = 0; i < file.value().size; ++i)
    {
        if (auto status = buffer.append(std::to_integer<char>(file.value().data[i])); !status.ok())
        {
            return status;
        }
    }
    if (buffer.size() + line.size() + 1 > fs::max_file_data)
    {
        buffer.clear();
        if (auto status = buffer.append("[kernel log truncated]\n"); !status.ok())
        {
            return status;
        }
    }
    if (auto status = buffer.append(line); !status.ok())
    {
        return status;
    }
    if (auto status = buffer.append('\n'); !status.ok())
    {
        return status;
    }
    return vfs_.write_file("/tmp/kernel.log", bytes_for(buffer.view()));
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
    if (auto status = append_kernel_log_line(line); !status.ok())
    {
        return status;
    }
    return display_driver_.write_line(line);
}

Status Kernel::log_daemon_restart(std::string_view kind, std::string_view process_name,
                                  sched::ProcessId previous_pid, sched::ProcessId pid)
{
    FixedString<160> line;
    if (auto status = line.assign("[ daemon ] "); !status.ok())
    {
        return status;
    }
    if (auto status = line.append(kind); !status.ok())
    {
        return status;
    }
    if (auto status = line.append(": restarted "); !status.ok())
    {
        return status;
    }
    if (auto status = line.append(process_name); !status.ok())
    {
        return status;
    }
    if (auto status = line.append(" pid="); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(line, previous_pid); !status.ok())
    {
        return status;
    }
    if (auto status = line.append(" -> "); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(line, pid); !status.ok())
    {
        return status;
    }
    return log_boot_line(line.view());
}

Status Kernel::register_builtin_interrupts(driver::TimerDriver &timer)
{
    return interrupts_.register_callback(32, "timer", &timer, [](void *context, arch::TrapFrame &) {
        static_cast<driver::TimerDriver *>(context)->tick();
        return Status::success();
    });
}

} // namespace ok
