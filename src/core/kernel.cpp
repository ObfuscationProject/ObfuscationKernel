#include "ok/core/kernel.hpp"
#include "ok/core/entry.hpp"
#include "ok/sched/process.hpp"

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

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

struct BootGuiModulePackage
{
    std::string_view path;
    std::string_view text;
};

constexpr std::string_view fallback_system_gui_okmod{
    "OKMOD\n"
    "name=system-gui\n"
    "version=1\n"
    "vermagic=okernel-cxx-oop\n"
    "require=gui.compositor\n"
    "require=gui.desktop\n"
    "export=gui.system-desktop\n"
    "param=entry:oop\n"
    "param=class:desktop\n"
    "param=brand:ObfuscationOS\n"
    "param=title:ObfuscationOS Login\n"
    "param=subtitle:choose root or user\n"
    "signature=system-gui-dev\n"};

constexpr BootGuiModulePackage boot_gui_module_fallbacks[] = {
    {"/boot/modules/system-gui.okmod", fallback_system_gui_okmod},
};

struct SystemGuiAppDefinition
{
    ExternalGuiDockApp app{ExternalGuiDockApp::none};
    std::string_view id{};
    std::string_view path{};
    std::string_view process_name{};
    std::string_view title{};
    std::string_view subtitle{};
    std::string_view body{};
    std::string_view command{};
    std::string_view line1{};
    std::string_view line2{};
    std::string_view line3{};
    gui::Rect bounds{};
    std::string_view accent{};
};

constexpr SystemGuiAppDefinition system_gui_apps[] = {
    {.app = ExternalGuiDockApp::shell,
     .id = "shell",
     .path = "/bin/oksh",
     .process_name = "app:oksh",
     .title = "Tiny Shell",
     .subtitle = "tiny-c shell ELF",
     .body = "Standalone user app",
     .command = "/bin/oksh",
     .line1 = "help ls cat stat pwd cd",
     .line2 = "whoami uname uptime touch",
     .line3 = "runs as the selected user",
     .bounds = gui::Rect{.x = 54, .y = 52, .width = 352, .height = 170},
     .accent = "blue"},
    {.app = ExternalGuiDockApp::settings,
     .id = "settings",
     .path = "/bin/settings",
     .process_name = "app:settings",
     .title = "System Settings",
     .subtitle = "settings ELF",
     .body = "Display, input, session",
     .command = "/bin/settings",
     .line1 = "palette mint / blue / rose",
     .line2 = "pointer and keyboard enabled",
     .line3 = "user session controls",
     .bounds = gui::Rect{.x = 520, .y = 56, .width = 344, .height = 158},
     .accent = "mint"},
    {.app = ExternalGuiDockApp::tasks,
     .id = "tasks",
     .path = "/bin/tasks",
     .process_name = "app:tasks",
     .title = "Task Manager",
     .subtitle = "task manager ELF",
     .body = "Live scheduler snapshot",
     .command = "/bin/tasks",
     .line3 = "kernel tools stay outside System dock",
     .bounds = gui::Rect{.x = 64, .y = 282, .width = 346, .height = 150},
     .accent = "violet"},
    {.app = ExternalGuiDockApp::notes,
     .id = "notes",
     .path = "/bin/notes",
     .process_name = "app:notes",
     .title = "Notes",
     .subtitle = "notes ELF",
     .body = "Small user scratchpad",
     .command = "/bin/notes",
     .line1 = "notes are owned by userland",
     .line2 = "CLI tools use libokcrt",
     .bounds = gui::Rect{.x = 520, .y = 278, .width = 346, .height = 150},
     .accent = "rose"},
    {.app = ExternalGuiDockApp::about,
     .id = "about",
     .path = "/bin/about",
     .process_name = "app:about",
     .title = "About ObfuscationOS",
     .subtitle = "about ELF",
     .body = "ObfuscationOS user desktop",
     .command = "/bin/about",
     .line1 = "standalone tiny-c app",
     .line2 = "launched from /bin",
     .bounds = gui::Rect{.x = 300, .y = 168, .width = 336, .height = 154},
     .accent = "gold"},
};

Result<SystemGuiAppDefinition> system_gui_app_definition(ExternalGuiDockApp app)
{
    for (const auto &definition : system_gui_apps)
    {
        if (definition.app == app)
        {
            return definition;
        }
    }
    return Status::not_found("system GUI app definition not found");
}

Result<fs::FileBuffer> boot_gui_module_fallback(std::string_view path)
{
    for (const auto &package : boot_gui_module_fallbacks)
    {
        if (package.path != path)
        {
            continue;
        }
        if (package.text.size() > fs::max_file_data)
        {
            return Status::overflow("boot GUI module fallback exceeds file buffer");
        }
        fs::FileBuffer file{};
        for (usize i = 0; i < package.text.size(); ++i)
        {
            file.data[i] = static_cast<std::byte>(package.text[i]);
        }
        file.size = package.text.size();
        return file;
    }
    return Status::not_found("boot GUI module fallback not found");
}

Result<FixedString<fs::simplefs_name_capacity>> simplefs_flat_rootfs_path(std::string_view path)
{
    if (path.empty())
    {
        return Status::invalid_argument("rootfs path is empty");
    }
    if (path.front() == '/')
    {
        path.remove_prefix(1);
    }
    FixedString<fs::simplefs_name_capacity> out;
    constexpr std::string_view bin_prefix{"bin/"};
    constexpr std::string_view etc_prefix{"etc/"};
    constexpr std::string_view apps_prefix{"boot/modules/apps/"};
    constexpr std::string_view modules_prefix{"boot/modules/"};
    if (starts_with(path, bin_prefix))
    {
        if (auto status = out.assign(path.substr(bin_prefix.size())); !status.ok())
        {
            return status;
        }
        return out;
    }
    if (starts_with(path, etc_prefix))
    {
        if (auto status = out.assign(path.substr(etc_prefix.size())); !status.ok())
        {
            return status;
        }
        return out;
    }
    if (starts_with(path, apps_prefix))
    {
        if (auto status = out.append("apps_"); !status.ok())
        {
            return status;
        }
        if (auto status = out.append(path.substr(apps_prefix.size())); !status.ok())
        {
            return status;
        }
        return out;
    }
    if (starts_with(path, modules_prefix))
    {
        if (auto status = out.assign(path.substr(modules_prefix.size())); !status.ok())
        {
            return status;
        }
        return out;
    }
    for (const auto value : path)
    {
        if (auto status = out.append(value == '/' ? '_' : value); !status.ok())
        {
            return status;
        }
    }
    return out;
}

bool cpu_accepts_work(smp::CpuState state)
{
    return state == smp::CpuState::boot || state == smp::CpuState::online;
}

bool point_hits_window_close(const gui::SurfaceInfo &surface, i32 x, i32 y)
{
    if (surface.chrome != gui::SurfaceChrome::decorated || surface.bounds.width < 34)
    {
        return false;
    }
    const auto local_x = x - surface.bounds.x;
    const auto local_y = y - surface.bounds.y;
    if (local_x < 0 || local_y < 3 || local_y > 8)
    {
        return false;
    }
    const auto right = static_cast<i32>(surface.bounds.width) - local_x;
    return right >= 8 && right <= 13;
}

constexpr u64 task_manager_refresh_interval_ticks = 8;

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
    file_managers_.clear();
    inactive_file_manager_.mark_closed();
    active_file_manager_index_ = gui::max_gui_surfaces;
    task_managers_.clear();
    inactive_task_manager_.mark_closed();
    active_task_manager_index_ = gui::max_gui_surfaces;
    gui_close_attempts_.clear();
    debug_test_points_run_ = 0;
    kernel_tick_count_ = 0;
    task_manager_next_refresh_tick_ = 0;
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
    for (usize cpu = 1; cpu < topology_.cpu_count(); ++cpu)
    {
        const auto cpu_context = arch_->make_kernel_context(0x1000 + static_cast<uptr>(cpu) * 0x100,
                                                            0x8000 + static_cast<uptr>(cpu) * 0x1000);
        if (auto thread = scheduler_.create_thread(idle.value(), cpu_context); !thread)
        {
            return thread.status();
        }
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
    if (auto status = log_boot_line("[    0.000000] obfuscationos: C++23 kernel services online"); !status.ok())
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
    auto simplefs_status = simplefs_.mount(disk());
    if (!simplefs_status.ok())
    {
        if (auto status = simplefs_.format(disk(), "okroot"); !status.ok())
        {
            return status;
        }
        if (auto status = log_boot_line("[    0.000006] fs: simplefs formatted on block device"); !status.ok())
        {
            return status;
        }
    }
    else if (auto status = log_boot_line("[    0.000006] fs: simplefs mounted from boot block device"); !status.ok())
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
    if (auto status = compositor.handle_mouse_delta(delta_x, delta_y, left_button); !status.ok())
    {
        gui_mouse_left_down_ = left_button;
        return status;
    }
    const auto window_event = compositor.consume_window_event();
    if (window_event.kind != gui::WindowEventKind::none)
    {
        if (auto status = handle_gui_window_event(window_event); !status.ok())
        {
            gui_mouse_left_down_ = left_button;
            return status;
        }
    }
    if (auto status = debug_shell_.reconcile_gui_windows(); !status.ok())
    {
        gui_mouse_left_down_ = left_button;
        return status;
    }
    if (auto status = reconcile_file_managers(); !status.ok())
    {
        gui_mouse_left_down_ = left_button;
        return status;
    }
    if (auto status = reconcile_task_managers(); !status.ok())
    {
        gui_mouse_left_down_ = left_button;
        return status;
    }
    if (click && window_event.kind == gui::WindowEventKind::none)
    {
        if (auto *system_desktop = loaded_gui_desktop_module();
            system_desktop != nullptr && system_desktop->desktop_state() == ExternalGuiDesktopState::greeter)
        {
            if (auto status = system_desktop->handle_pointer_click(compositor.pointer_x(), compositor.pointer_y());
                !status.ok())
            {
                gui_mouse_left_down_ = left_button;
                return status;
            }
            if (system_desktop->desktop_state() == ExternalGuiDesktopState::desktop)
            {
                if (auto status = start_selected_system_gui_session(*system_desktop); !status.ok())
                {
                    gui_mouse_left_down_ = left_button;
                    return status;
                }
            }
            gui_mouse_left_down_ = left_button;
            return Status::success();
        }
        auto launcher = compositor.taskbar_launcher_at(compositor.pointer_x(), compositor.pointer_y());
        if (!launcher)
        {
            if (launcher.status().code() != StatusCode::not_found)
            {
                gui_mouse_left_down_ = left_button;
                return launcher.status();
            }
        }
        if (launcher && launcher.value() != gui::TaskbarApp::none)
        {
            if (auto status = handle_gui_taskbar_launcher(launcher.value()); !status.ok())
            {
                gui_mouse_left_down_ = left_button;
                return status;
            }
            gui_mouse_left_down_ = left_button;
            return Status::success();
        }
        const auto active = compositor.active_surface();
        const auto close_hit = compositor.surface_at(compositor.pointer_x(), compositor.pointer_y());
        const auto close_surface = close_hit ? close_hit.value() : active;
        if (auto close_info = compositor.surface_info(close_surface);
            close_info && point_hits_window_close(close_info.value(), compositor.pointer_x(), compositor.pointer_y()))
        {
            if (auto app = find_system_gui_app_by_surface(close_surface))
            {
                gui_mouse_left_down_ = left_button;
                return close_system_gui_app_at(app.value(), true);
            }
        }
        if (auto *system_desktop = loaded_gui_desktop_module();
            system_desktop != nullptr && system_desktop->desktop_state() == ExternalGuiDesktopState::desktop)
        {
            auto dock_launcher = system_desktop->dock_launcher_at(compositor.pointer_x(), compositor.pointer_y());
            if (!dock_launcher)
            {
                if (dock_launcher.status().code() != StatusCode::not_found)
                {
                    gui_mouse_left_down_ = left_button;
                    return dock_launcher.status();
                }
            }
            else if (dock_launcher.value() != ExternalGuiDockApp::none)
            {
                if (auto status = handle_system_gui_dock_launcher(dock_launcher.value()); !status.ok())
                {
                    gui_mouse_left_down_ = left_button;
                    return status;
                }
                gui_mouse_left_down_ = left_button;
                return Status::success();
            }
        }
        if (auto manager = find_file_manager_by_surface(active))
        {
            active_file_manager_index_ = manager.value();
            if (auto status = file_managers_[manager.value()].handle_mouse(compositor, vfs_, compositor.pointer_x(),
                                                                            compositor.pointer_y(), true);
                !status.ok())
            {
                gui_mouse_left_down_ = left_button;
                return status;
            }
        }
    }
    gui_mouse_left_down_ = left_button;
    return Status::success();
}

Status Kernel::handle_gui_mouse_position(i32 x, i32 y, bool left_button)
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
    if (auto status = compositor.set_pointer_position(x, y); !status.ok())
    {
        return status;
    }
    return handle_gui_mouse(0, 0, left_button);
}

Status Kernel::handle_gui_scroll(i32 rows)
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
    const auto active = compositor.active_surface();
    if (auto monitor = find_task_manager_by_surface(active))
    {
        active_task_manager_index_ = monitor.value();
        return task_managers_[monitor.value()].scroll_processes(compositor, *this, rows);
    }
    if (active != 0 && debug_shell_.owns_surface(active))
    {
        return debug_shell_.scroll_gui_history(rows);
    }
    return debug_shell_.scroll_gui_history(rows);
}

Status Kernel::handle_gui_key(int key)
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

    const auto active = compositor.active_surface();
    if (auto *system_desktop = loaded_gui_desktop_module();
        system_desktop != nullptr && system_desktop->desktop_state() == ExternalGuiDesktopState::greeter)
    {
        if (auto status = system_desktop->handle_key(key); !status.ok())
        {
            return status;
        }
        if (system_desktop->desktop_state() == ExternalGuiDesktopState::desktop)
        {
            return start_selected_system_gui_session(*system_desktop);
        }
        return Status::success();
    }
    if (key == ok_input_open_shell)
    {
        if (auto status = sync_gui_credentials_from_surface(active); !status.ok())
        {
            return status;
        }
        return debug_shell_.show_gui();
    }
    if (key == ok_input_open_file_manager)
    {
        if (auto status = sync_gui_credentials_from_surface(active); !status.ok())
        {
            return status;
        }
        return open_file_manager(posix_.getcwd(), false);
    }
    if (key == 0x03 && debug_shell_.foreground_process_id() != 0)
    {
        return debug_shell_.interrupt_foreground_process();
    }

    if (active != 0 && debug_shell_.owns_surface(active))
    {
        return debug_shell_.handle_key(active, key);
    }
    if (auto monitor = find_task_manager_by_surface(active))
    {
        active_task_manager_index_ = monitor.value();
        if (key == 0x03 && task_managers_[monitor.value()].process_id() != 0 &&
            debug_shell_.foreground_process_id() == task_managers_[monitor.value()].process_id())
        {
            return debug_shell_.interrupt_foreground_process();
        }
        return task_managers_[monitor.value()].handle_key(compositor, *this, key);
    }
    if (active != 0)
    {
        if (auto manager = find_file_manager_by_surface(active))
        {
            active_file_manager_index_ = manager.value();
            return file_managers_[manager.value()].handle_key(compositor, vfs_, key);
        }
    }
    return Status::success();
}

Status Kernel::handle_gui_taskbar_launcher(gui::TaskbarApp app)
{
    switch (app)
    {
    case gui::TaskbarApp::debug_shell:
        if (auto status = sync_gui_credentials_from_surface(gui_module_.compositor().active_surface()); !status.ok())
        {
            return status;
        }
        return debug_shell_.show_gui();
    case gui::TaskbarApp::file_manager:
        if (auto status = sync_gui_credentials_from_surface(gui_module_.compositor().active_surface()); !status.ok())
        {
            return status;
        }
        return open_file_manager(posix_.getcwd(), false);
    case gui::TaskbarApp::task_monitor:
        if (auto status = sync_gui_credentials_from_surface(gui_module_.compositor().active_surface()); !status.ok())
        {
            return status;
        }
        return open_task_manager();
    case gui::TaskbarApp::none:
        return Status::success();
    }
    return Status::success();
}

Status Kernel::start_selected_system_gui_session(ExternalGuiDesktopModule &desktop)
{
    auto credentials = user_space_.credentials_for(desktop.selected_login_user_name());
    if (!credentials)
    {
        return credentials.status();
    }
    if (auto status = posix_.set_credentials(credentials.value()); !status.ok())
    {
        return status;
    }
    return launch_system_gui_app_session();
}

Status Kernel::handle_system_gui_dock_launcher(ExternalGuiDockApp app)
{
    return app == ExternalGuiDockApp::none ? Status::success() : focus_system_gui_app(app);
}

Status Kernel::focus_system_gui_app(ExternalGuiDockApp app)
{
    auto definition = system_gui_app_definition(app);
    if (!definition)
    {
        return definition.status();
    }
    for (auto &instance : external_gui_app_modules_)
    {
        if (!instance.configured() || instance.app_id() != definition.value().id)
        {
            continue;
        }
        if (instance.process_id() == 0 || scheduler_.find(instance.process_id()) == nullptr)
        {
            instance.reset();
            break;
        }
        if (auto status = instance.refresh(); !status.ok())
        {
            return status;
        }
        auto info = gui_module_.compositor().surface_info(instance.surface());
        if (!info)
        {
            return info.status();
        }
        if (auto status = gui_module_.compositor().raise_surface(instance.surface()); !status.ok())
        {
            return status;
        }
        return gui_module_.compositor().present();
    }
    return launch_system_gui_app(app);
}

Status Kernel::tick()
{
    if (!booted_)
    {
        return Status::not_initialized("kernel is not booted");
    }
    ++kernel_tick_count_;

    for (usize i = 0; i < topology_.cpu_count(); ++i)
    {
        const auto cpu = static_cast<smp::CpuId>(i);
        const auto *info = topology_.cpu(cpu);
        if (info == nullptr || !cpu_accepts_work(info->state))
        {
            continue;
        }
        auto selected = scheduler_.schedule_next_on_cpu(cpu);
        if (!selected)
        {
            if (selected.status().code() == StatusCode::would_block)
            {
                continue;
            }
            return selected.status();
        }
        if (auto status = topology_.record_schedule(cpu); !status.ok())
        {
            return status;
        }
    }

    if (auto status = debug_shell_.tick(); !status.ok())
    {
        return status;
    }
    if (!task_managers_.empty())
    {
        if (kernel_tick_count_ < task_manager_next_refresh_tick_)
        {
            return Status::success();
        }
        for (usize i = 0; i < task_managers_.size(); ++i)
        {
            if (task_managers_[i].surface_id() == 0 || task_managers_[i].process_id() == 0 ||
                scheduler_.find(task_managers_[i].process_id()) == nullptr)
            {
                continue;
            }
            if (auto status = task_managers_[i].refresh(gui_module_.compositor(), *this); !status.ok())
            {
                return status;
            }
        }
        if (auto status = reconcile_task_managers(); !status.ok())
        {
            return status;
        }
        task_manager_next_refresh_tick_ = kernel_tick_count_ + task_manager_refresh_interval_ticks;
    }
    return Status::success();
}

Status Kernel::sync_gui_credentials_from_surface(gui::SurfaceId surface)
{
    if (surface == 0)
    {
        return Status::success();
    }
    if (debug_shell_.owns_surface(surface))
    {
        return debug_shell_.handle_surface_changed(surface);
    }
    if (auto manager = find_file_manager_by_surface(surface))
    {
        active_file_manager_index_ = manager.value();
        return posix_.set_credentials(file_managers_[manager.value()].credentials());
    }
    if (auto monitor = find_task_manager_by_surface(surface))
    {
        active_task_manager_index_ = monitor.value();
        return posix_.set_credentials(task_managers_[monitor.value()].credentials());
    }
    if (auto app = find_system_gui_app_by_surface(surface))
    {
        const auto process = external_gui_app_modules_[app.value()].process_id();
        const auto *pcb = scheduler_.find(process);
        return pcb == nullptr ? Status::success() : posix_.set_credentials(pcb->credentials());
    }
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
        if (auto manager = find_file_manager_by_surface(event.surface_id))
        {
            active_file_manager_index_ = manager.value();
            return file_managers_[manager.value()].handle_surface_changed(gui_module_.compositor(), vfs_);
        }
        if (auto monitor = find_task_manager_by_surface(event.surface_id))
        {
            active_task_manager_index_ = monitor.value();
            return gui_module_.compositor().present();
        }
        if (auto app = find_system_gui_app_by_surface(event.surface_id))
        {
            return external_gui_app_modules_[app.value()].refresh();
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
    else if (auto manager = find_file_manager_by_surface(surface))
    {
        status = close_file_manager_at(manager.value(), true, true);
    }
    else if (auto monitor = find_task_manager_by_surface(surface))
    {
        status = close_task_manager_at(monitor.value(), true, true);
    }
    else if (auto app = find_system_gui_app_by_surface(surface))
    {
        status = close_system_gui_app_at(app.value(), true);
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
    if (auto manager = find_file_manager_by_surface(surface))
    {
        active_file_manager_index_ = manager.value();
        return file_managers_[manager.value()].handle_surface_changed(gui_module_.compositor(), vfs_);
    }
    if (auto monitor = find_task_manager_by_surface(surface))
    {
        active_task_manager_index_ = monitor.value();
        return task_managers_[monitor.value()].refresh(gui_module_.compositor(), *this);
    }
    if (auto app = find_system_gui_app_by_surface(surface))
    {
        return external_gui_app_modules_[app.value()].refresh();
    }
    return gui_module_.compositor().present();
}

Status Kernel::force_close_gui_surface(gui::SurfaceId surface)
{
    if (debug_shell_.owns_surface(surface))
    {
        return debug_shell_.close_surface_window(surface);
    }
    if (auto manager = find_file_manager_by_surface(surface))
    {
        return close_file_manager_at(manager.value(), true, true);
    }
    if (auto monitor = find_task_manager_by_surface(surface))
    {
        return close_task_manager_at(monitor.value(), true, true);
    }
    if (auto app = find_system_gui_app_by_surface(surface))
    {
        return close_system_gui_app_at(app.value(), true);
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

Result<fs::FileBuffer> Kernel::read_external_module_file(std::string_view path)
{
    auto file = vfs_.read_file(path);
    if (file || file.status().code() != StatusCode::not_found)
    {
        return file;
    }
    if (!simplefs_.mounted())
    {
        return boot_gui_module_fallback(path);
    }
    auto flat_path = simplefs_flat_rootfs_path(path);
    if (!flat_path)
    {
        return boot_gui_module_fallback(path);
    }
    auto simplefs_file = simplefs_.read_file(flat_path.value().view());
    if (simplefs_file || simplefs_file.status().code() != StatusCode::not_found)
    {
        return simplefs_file;
    }
    return boot_gui_module_fallback(path);
}

Result<fs::FileBuffer> Kernel::read_user_program_file(std::string_view path)
{
    auto file = vfs_.read_file(path);
    if (file || file.status().code() != StatusCode::not_found)
    {
        return file;
    }
    if (!simplefs_.mounted())
    {
        return file.status();
    }
    auto flat_path = simplefs_flat_rootfs_path(path);
    if (!flat_path)
    {
        return flat_path.status();
    }
    return simplefs_.read_file(flat_path.value().view());
}

Status Kernel::load_external_gui_desktop_module(std::string_view path, const ModuleImageInfo &image)
{
    external_gui_desktop_module_.bind_metrics(scheduler_, topology_);
    if (auto status = external_gui_desktop_module_.configure_from_image(path, image); !status.ok())
    {
        return status;
    }
    auto *module = kernel_modules_.find(external_gui_desktop_module_.module_name());
    if (module == nullptr)
    {
        if (auto status = kernel_modules_.register_module(external_gui_desktop_module_); !status.ok())
        {
            return status;
        }
    }
    return kernel_modules_.start_registered_module(external_gui_desktop_module_.module_name());
}

Status Kernel::launch_system_gui_app(ExternalGuiDockApp app)
{
    if (external_gui_desktop_module_.configured() &&
        external_gui_desktop_module_.desktop_state() == ExternalGuiDesktopState::greeter)
    {
        return Status::would_block("system GUI apps are blocked until login");
    }
    auto definition = system_gui_app_definition(app);
    if (!definition)
    {
        return definition.status();
    }
    auto program = read_user_program_file(definition.value().path);
    if (!program)
    {
        return program.status();
    }
    sched::ElfLoader loader;
    auto loaded = loader.load(std::span<const std::byte>{program.value().data.data(), program.value().size},
                              arch_->architecture());
    if (!loaded)
    {
        return loaded.status();
    }

    ExternalGuiAppModule *slot = nullptr;
    for (auto &instance : external_gui_app_modules_)
    {
        if (!instance.configured())
        {
            slot = &instance;
            break;
        }
    }
    if (slot == nullptr)
    {
        return Status::overflow("system GUI app table is full");
    }

    const auto credentials = posix_.user_credentials();
    auto process = create_ui_process(definition.value().process_name, loaded.value().entry, loaded.value().stack_pointer,
                                     credentials);
    if (!process)
    {
        return process.status();
    }

    slot->bind_metrics(scheduler_, topology_);
    if (auto status = slot->configure_from_elf(definition.value().id, definition.value().path, definition.value().title,
                                               definition.value().subtitle, definition.value().body,
                                               definition.value().command, definition.value().line1,
                                               definition.value().line2, definition.value().line3,
                                               definition.value().bounds, definition.value().accent, process.value());
        !status.ok())
    {
        static_cast<void>(scheduler_.kill_process(process.value()));
        slot->reset();
        return status;
    }
    if (auto status = slot->start(gui_module_.compositor(), gui_module_.desktop()); !status.ok())
    {
        static_cast<void>(scheduler_.kill_process(process.value()));
        slot->reset();
        return status;
    }
    return Status::success();
}

Status Kernel::launch_system_gui_app_session()
{
    for (const auto &definition : system_gui_apps)
    {
        if (auto status = focus_system_gui_app(definition.app); !status.ok() && status.code() != StatusCode::not_found)
        {
            return status;
        }
    }
    return refresh_external_gui_modules(false);
}

Status Kernel::load_external_kernel_module(std::string_view path)
{
    if (arch_ == nullptr)
    {
        return Status::not_initialized("kernel architecture is not selected");
    }
    auto file = read_external_module_file(path);
    if (!file)
    {
        return file.status();
    }
    ModuleImageLoader loader;
    auto image = loader.parse(std::span<const std::byte>{file.value().data.data(), file.value().size},
                              arch_->architecture());
    if (!image)
    {
        return image.status();
    }
    auto *module = kernel_modules_.find(image.value().name.view());
    if (module != nullptr)
    {
        return module->state() == ModuleState::started ? Status::success()
                                                       : kernel_modules_.start_registered_module(image.value().name.view());
    }
    auto desktop_status = load_external_gui_desktop_module(path, image.value());
    if (desktop_status.ok())
    {
        return desktop_status;
    }
    if (desktop_status.code() != StatusCode::unsupported)
    {
        return desktop_status;
    }
    return Status::unsupported("GUI app OKMOD packages are disabled; launch a user ELF app instead");
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

Result<sched::ProcessId> Kernel::create_ui_process(std::string_view name, uptr entry, uptr stack_top,
                                                   user::Credentials credentials)
{
    auto process = credentials.kernel_space
                       ? scheduler_.create_process(name, arch_->make_kernel_context(entry, stack_top))
                       : scheduler_.create_user_process(
                             name, arch_->make_user_context(arch::UserEntry{
                                       .instruction_pointer = entry,
                                       .stack_pointer = stack_top,
                                       .argument = 0,
                                   }));
    if (!process)
    {
        return process.status();
    }
    if (auto status = scheduler_.set_credentials(process.value(), credentials); !status.ok())
    {
        static_cast<void>(scheduler_.kill_process(process.value()));
        return status;
    }
    if (auto status = scheduler_.set_runnable(process.value()); !status.ok())
    {
        static_cast<void>(scheduler_.kill_process(process.value()));
        return status;
    }
    return process.value();
}

Result<usize> Kernel::find_file_manager_by_surface(gui::SurfaceId surface) const
{
    for (usize i = 0; i < file_managers_.size(); ++i)
    {
        if (file_managers_[i].surface_id() == surface)
        {
            return i;
        }
    }
    return Status::not_found("file manager surface not found");
}

Result<usize> Kernel::find_file_manager_by_process(sched::ProcessId pid) const
{
    for (usize i = 0; i < file_managers_.size(); ++i)
    {
        if (file_managers_[i].process_id() == pid)
        {
            return i;
        }
    }
    return Status::not_found("file manager process not found");
}

Result<usize> Kernel::find_task_manager_by_surface(gui::SurfaceId surface) const
{
    for (usize i = 0; i < task_managers_.size(); ++i)
    {
        if (task_managers_[i].surface_id() == surface)
        {
            return i;
        }
    }
    return Status::not_found("task monitor surface not found");
}

Result<usize> Kernel::find_task_manager_by_process(sched::ProcessId pid) const
{
    for (usize i = 0; i < task_managers_.size(); ++i)
    {
        if (task_managers_[i].process_id() == pid)
        {
            return i;
        }
    }
    return Status::not_found("task monitor process not found");
}

Result<usize> Kernel::find_system_gui_app_by_surface(gui::SurfaceId surface) const
{
    for (usize i = 0; i < external_gui_app_modules_.size(); ++i)
    {
        if (external_gui_app_modules_[i].configured() && external_gui_app_modules_[i].surface() == surface)
        {
            return i;
        }
    }
    return Status::not_found("system GUI app surface not found");
}

Result<usize> Kernel::find_system_gui_app_by_process(sched::ProcessId pid) const
{
    for (usize i = 0; i < external_gui_app_modules_.size(); ++i)
    {
        if (external_gui_app_modules_[i].configured() && external_gui_app_modules_[i].process_id() == pid)
        {
            return i;
        }
    }
    return Status::not_found("system GUI app process not found");
}

Status Kernel::close_system_gui_app_at(usize index, bool kill_process)
{
    if (index >= external_gui_app_modules_.size() || !external_gui_app_modules_[index].configured())
    {
        return Status::invalid_argument("system GUI app index out of range");
    }
    auto &app = external_gui_app_modules_[index];
    const auto process = app.process_id();
    const auto surface = app.surface();
    if (surface != 0)
    {
        clear_gui_close_attempt(surface);
    }
    if (auto status = app.stop(); !status.ok() && status.code() != StatusCode::not_found)
    {
        app.reset();
        return status;
    }
    if (kill_process && process != 0 && scheduler_.find(process) != nullptr)
    {
        static_cast<void>(scheduler_.kill_process(process));
    }
    if (process != 0)
    {
        debug_shell_.notify_process_exit(process);
    }
    app.reset();
    return gui_module_.compositor().present();
}

Status Kernel::close_file_manager_at(usize index, bool kill_process, bool notify_shell)
{
    if (index >= file_managers_.size())
    {
        return Status::invalid_argument("file manager window index out of range");
    }
    auto &manager = file_managers_[index];
    const auto process = manager.process_id();
    const auto surface = manager.surface_id();
    if (surface != 0)
    {
        clear_gui_close_attempt(surface);
    }
    if (auto status = manager.close(gui_module_.compositor()); !status.ok() && status.code() != StatusCode::not_found)
    {
        manager.mark_closed();
        return status;
    }
    if (kill_process && process != 0 && scheduler_.find(process) != nullptr)
    {
        static_cast<void>(scheduler_.kill_process(process));
    }
    if (notify_shell && process != 0)
    {
        debug_shell_.notify_process_exit(process);
    }
    const auto previous_active = active_file_manager_index_;
    if (auto status = file_managers_.erase_at(index); !status.ok())
    {
        return status;
    }
    if (file_managers_.empty())
    {
        active_file_manager_index_ = gui::max_gui_surfaces;
    }
    else if (previous_active == index)
    {
        active_file_manager_index_ = index < file_managers_.size() ? index : file_managers_.size() - 1;
    }
    else if (previous_active > index)
    {
        active_file_manager_index_ = previous_active - 1;
    }
    else if (previous_active < file_managers_.size())
    {
        active_file_manager_index_ = previous_active;
    }
    else
    {
        active_file_manager_index_ = file_managers_.size() - 1;
    }
    return Status::success();
}

Status Kernel::close_all_file_managers()
{
    for (usize i = file_managers_.size(); i != 0; --i)
    {
        if (auto status = close_file_manager_at(i - 1, true, true); !status.ok())
        {
            return status;
        }
    }
    inactive_file_manager_.mark_closed();
    active_file_manager_index_ = gui::max_gui_surfaces;
    return Status::success();
}

Status Kernel::reconcile_file_managers()
{
    auto &compositor = gui_module_.compositor();
    for (usize i = 0; i < file_managers_.size();)
    {
        const auto surface = file_managers_[i].surface_id();
        const auto process = file_managers_[i].process_id();
        const bool surface_alive = surface != 0 && compositor.surface_info(surface);
        const bool process_alive = process != 0 && scheduler_.find(process) != nullptr;
        if (surface_alive && process_alive)
        {
            ++i;
            continue;
        }
        if (process_alive)
        {
            static_cast<void>(scheduler_.kill_process(process));
        }
        if (surface_alive)
        {
            static_cast<void>(file_managers_[i].close(compositor));
        }
        if (process != 0)
        {
            debug_shell_.notify_process_exit(process);
        }
        const auto removed = i;
        static_cast<void>(file_managers_.erase_at(i));
        if (active_file_manager_index_ > removed)
        {
            --active_file_manager_index_;
        }
    }
    if (file_managers_.empty())
    {
        active_file_manager_index_ = gui::max_gui_surfaces;
    }
    else if (active_file_manager_index_ >= file_managers_.size())
    {
        active_file_manager_index_ = file_managers_.size() - 1;
    }
    return Status::success();
}

Status Kernel::reconcile_task_managers()
{
    auto &compositor = gui_module_.compositor();
    for (usize i = 0; i < task_managers_.size();)
    {
        const auto surface = task_managers_[i].surface_id();
        const auto process = task_managers_[i].process_id();
        const bool surface_alive = surface != 0 && compositor.surface_info(surface);
        const bool process_alive = process != 0 && scheduler_.find(process) != nullptr;
        if (surface_alive && process_alive)
        {
            ++i;
            continue;
        }
        if (process_alive)
        {
            static_cast<void>(scheduler_.kill_process(process));
        }
        if (surface_alive)
        {
            static_cast<void>(task_managers_[i].close(compositor));
        }
        if (process != 0)
        {
            debug_shell_.notify_process_exit(process);
        }
        const auto removed = i;
        static_cast<void>(task_managers_.erase_at(i));
        if (active_task_manager_index_ > removed)
        {
            --active_task_manager_index_;
        }
    }
    if (task_managers_.empty())
    {
        active_task_manager_index_ = gui::max_gui_surfaces;
        task_manager_next_refresh_tick_ = 0;
    }
    else if (active_task_manager_index_ >= task_managers_.size())
    {
        active_task_manager_index_ = task_managers_.size() - 1;
    }
    return Status::success();
}

Status Kernel::open_file_manager(std::string_view path, bool foreground_shell_child)
{
    if (!booted_)
    {
        return Status::not_initialized("kernel is not booted");
    }
    if (auto status = reconcile_file_managers(); !status.ok())
    {
        return status;
    }
    if (file_managers_.full())
    {
        return Status::overflow("file manager window table is full");
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
    auto process = create_ui_process(process_name.view(), 0x7000 + process_offset, 0xe000 + process_offset, credentials);
    if (!process)
    {
        return process.status();
    }

    apps::KernelFileManager manager;
    if (auto status = manager.open(gui_module_.compositor(), vfs_, path, credentials, process.value()); !status.ok())
    {
        static_cast<void>(manager.close(gui_module_.compositor()));
        static_cast<void>(scheduler_.kill_process(process.value()));
        return status;
    }
    if (auto status = file_managers_.push_back(manager); !status.ok())
    {
        static_cast<void>(manager.close(gui_module_.compositor()));
        static_cast<void>(scheduler_.kill_process(process.value()));
        return status;
    }
    active_file_manager_index_ = file_managers_.size() - 1;
    if (foreground_shell_child)
    {
        if (auto status = debug_shell_.start_foreground_process(process.value()); !status.ok())
        {
            static_cast<void>(close_file_manager_at(active_file_manager_index_, true, true));
            return status;
        }
    }
    return Status::success();
}

Status Kernel::open_task_manager(bool foreground_shell_child, std::string_view program_name)
{
    if (!booted_)
    {
        return Status::not_initialized("kernel is not booted");
    }
    if (auto status = reconcile_task_managers(); !status.ok())
    {
        return status;
    }
    if (task_managers_.full())
    {
        return Status::overflow("task monitor window table is full");
    }

    const auto credentials = posix_.user_credentials();
    FixedString<sched::max_process_name> process_name;
    if (auto status = process_name.assign(program_name == "top" ? "top:" : "tm:"); !status.ok())
    {
        return status;
    }
    if (auto status = process_name.append(user_label_for(user_space_, credentials)); !status.ok())
    {
        return status;
    }

    const auto process_offset = scheduler_.process_count() * 0x1000;
    auto process = create_ui_process(process_name.view(), 0x7800 + process_offset, 0xf000 + process_offset, credentials);
    if (!process)
    {
        return process.status();
    }

    const auto program = program_name == "top" ? apps::TaskMonitorProgram::top : apps::TaskMonitorProgram::task_manager;
    apps::KernelTaskManager manager;
    if (auto status = manager.open(gui_module_.compositor(), *this, credentials, process.value(), program); !status.ok())
    {
        static_cast<void>(manager.close(gui_module_.compositor()));
        static_cast<void>(scheduler_.kill_process(process.value()));
        return status;
    }
    if (auto status = task_managers_.push_back(manager); !status.ok())
    {
        static_cast<void>(manager.close(gui_module_.compositor()));
        static_cast<void>(scheduler_.kill_process(process.value()));
        return status;
    }
    active_task_manager_index_ = task_managers_.size() - 1;
    task_manager_next_refresh_tick_ = kernel_tick_count_ + 1;
    if (foreground_shell_child)
    {
        if (auto status = debug_shell_.start_foreground_process(process.value()); !status.ok())
        {
            static_cast<void>(close_task_manager_at(active_task_manager_index_, true, true));
            return status;
        }
    }
    return Status::success();
}

Status Kernel::close_file_manager()
{
    if (active_file_manager_index_ >= file_managers_.size())
    {
        return Status::success();
    }
    return close_file_manager_at(active_file_manager_index_, true, true);
}

Status Kernel::close_task_manager()
{
    if (active_task_manager_index_ >= task_managers_.size())
    {
        return Status::success();
    }
    return close_task_manager_at(active_task_manager_index_, true, true);
}

Status Kernel::close_task_manager_at(usize index, bool kill_process, bool notify_shell)
{
    if (index >= task_managers_.size())
    {
        return Status::invalid_argument("task monitor window index out of range");
    }
    auto &manager = task_managers_[index];
    const auto process = manager.process_id();
    const auto surface = manager.surface_id();
    if (surface != 0)
    {
        clear_gui_close_attempt(surface);
    }
    if (auto status = manager.close(gui_module_.compositor()); !status.ok() &&
                       status.code() != StatusCode::not_found)
    {
        manager.mark_closed();
        return status;
    }
    if (kill_process && process != 0 && scheduler_.find(process) != nullptr)
    {
        static_cast<void>(scheduler_.kill_process(process));
    }
    if (notify_shell && process != 0)
    {
        debug_shell_.notify_process_exit(process);
    }
    const auto previous_active = active_task_manager_index_;
    if (auto status = task_managers_.erase_at(index); !status.ok())
    {
        return status;
    }
    if (task_managers_.empty())
    {
        active_task_manager_index_ = gui::max_gui_surfaces;
        task_manager_next_refresh_tick_ = 0;
    }
    else if (previous_active == index)
    {
        active_task_manager_index_ = index < task_managers_.size() ? index : task_managers_.size() - 1;
    }
    else if (previous_active > index)
    {
        active_task_manager_index_ = previous_active - 1;
    }
    else if (previous_active < task_managers_.size())
    {
        active_task_manager_index_ = previous_active;
    }
    else
    {
        active_task_manager_index_ = task_managers_.size() - 1;
    }
    return Status::success();
}

Status Kernel::close_all_task_managers()
{
    for (usize i = task_managers_.size(); i != 0; --i)
    {
        if (auto status = close_task_manager_at(i - 1, true, true); !status.ok())
        {
            return status;
        }
    }
    inactive_task_manager_.mark_closed();
    active_task_manager_index_ = gui::max_gui_surfaces;
    task_manager_next_refresh_tick_ = 0;
    return Status::success();
}

Status Kernel::focus_file_manager()
{
    if (active_file_manager_index_ >= file_managers_.size())
    {
        return Status::not_initialized("file manager surface is not open");
    }
    return focus_file_manager_at(active_file_manager_index_);
}

Status Kernel::focus_file_manager_at(usize index)
{
    auto &compositor = gui_module_.compositor();
    if (index >= file_managers_.size())
    {
        return Status::invalid_argument("file manager window index out of range");
    }
    active_file_manager_index_ = index;
    auto &manager = file_managers_[index];
    const auto surface = manager.surface_id();
    if (surface == 0)
    {
        return Status::not_initialized("file manager surface is not open");
    }
    auto info = compositor.surface_info(surface);
    if (!info)
    {
        return close_file_manager_at(index, true, true);
    }
    if (info.value().window_state == gui::WindowState::minimized)
    {
        if (auto status = compositor.restore_surface(surface); !status.ok())
        {
            return status;
        }
    }
    else if (auto status = compositor.raise_surface(surface); !status.ok())
    {
        return status;
    }
    return manager.handle_surface_changed(compositor, vfs_);
}

Status Kernel::focus_task_manager()
{
    if (active_task_manager_index_ >= task_managers_.size())
    {
        return Status::not_initialized("task monitor surface is not open");
    }
    return focus_task_manager_at(active_task_manager_index_);
}

Status Kernel::focus_task_manager_at(usize index)
{
    auto &compositor = gui_module_.compositor();
    if (index >= task_managers_.size())
    {
        return Status::invalid_argument("task monitor window index out of range");
    }
    active_task_manager_index_ = index;
    auto &manager = task_managers_[index];
    const auto surface = manager.surface_id();
    if (surface == 0)
    {
        return Status::not_initialized("task monitor surface is not open");
    }
    auto info = compositor.surface_info(surface);
    if (!info)
    {
        return close_task_manager_at(index, true, true);
    }
    if (info.value().window_state == gui::WindowState::minimized)
    {
        if (auto status = compositor.restore_surface(surface); !status.ok())
        {
            return status;
        }
    }
    else if (auto status = compositor.raise_surface(surface); !status.ok())
    {
        return status;
    }
    return manager.refresh(compositor, *this);
}

Status Kernel::close_debug_gui()
{
    if (auto status = close_all_task_managers(); !status.ok())
    {
        return status;
    }
    if (auto status = close_all_file_managers(); !status.ok())
    {
        return status;
    }
    if (auto status = debug_shell_.close_all_gui(); !status.ok())
    {
        return status;
    }
    auto &compositor = gui_module_.compositor();
    if (compositor.state() == gui::GuiState::running)
    {
        return refresh_external_gui_modules(true);
    }
    return Status::success();
}

Status Kernel::refresh_external_gui_modules(bool focus_desktop)
{
    auto &compositor = gui_module_.compositor();
    if (compositor.state() != gui::GuiState::running)
    {
        return Status::success();
    }
    auto *desktop = loaded_gui_desktop_module();
    if (desktop != nullptr && (desktop->desktop_state() == ExternalGuiDesktopState::greeter ||
                               desktop->desktop_state() == ExternalGuiDesktopState::desktop))
    {
        if (auto status = desktop->refresh(); !status.ok())
        {
            return status;
        }
        if (focus_desktop && desktop->desktop_state() == ExternalGuiDesktopState::greeter &&
            desktop->dashboard_surface() != 0)
        {
            if (auto info = compositor.surface_info(desktop->dashboard_surface()))
            {
                if (info.value().window_state == gui::WindowState::minimized)
                {
                    if (auto status = compositor.restore_surface(desktop->dashboard_surface()); !status.ok())
                    {
                        return status;
                    }
                }
                if (auto status = compositor.raise_surface(desktop->dashboard_surface()); !status.ok())
                {
                    return status;
                }
            }
        }
    }
    for (auto &app : external_gui_app_modules_)
    {
        if (app.configured() && app.app_state() == ExternalGuiAppState::running)
        {
            if (auto status = app.refresh(); !status.ok())
            {
                return status;
            }
        }
    }
    return compositor.present();
}

Status Kernel::request_power_action(SystemPowerAction action)
{
    if (action == SystemPowerAction::none)
    {
        return Status::invalid_argument("power action must not be none");
    }
    if (power_action_ != SystemPowerAction::none && power_action_ != action)
    {
        return Status::busy("a power action is already pending");
    }
    power_action_ = action;
    return Status::success();
}

Status Kernel::kill_process(sched::ProcessId pid)
{
    if (pid == 0)
    {
        return Status::invalid_argument("process id must be non-zero");
    }
    const bool shell_process = debug_shell_.owns_process(pid);
    const auto file_manager_process = find_file_manager_by_process(pid);
    const auto task_manager_process = find_task_manager_by_process(pid);
    const auto system_app_process = find_system_gui_app_by_process(pid);
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
        if (auto status = close_file_manager_at(file_manager_process.value(), false, false);
            !status.ok() && status.code() != StatusCode::not_found)
        {
            return status;
        }
    }
    if (task_manager_process)
    {
        if (auto status = close_task_manager_at(task_manager_process.value(), false, false);
            !status.ok() && status.code() != StatusCode::not_found)
        {
            return status;
        }
    }
    if (system_app_process)
    {
        if (auto status = close_system_gui_app_at(system_app_process.value(), false);
            !status.ok() && status.code() != StatusCode::not_found)
        {
            return status;
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
    if (!module_restarts.empty())
    {
        if (auto status = refresh_external_gui_modules(false); !status.ok())
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
