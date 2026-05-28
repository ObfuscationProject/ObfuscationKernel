#pragma once

#include "ok/apps/file_manager.hpp"
#include "ok/apps/task_manager.hpp"
#include "ok/arch/arch.hpp"
#include "ok/core/power.hpp"
#include "ok/apps/shell.hpp"
#include "ok/driver/driver.hpp"
#include "ok/core/external_module.hpp"
#include "ok/fs/simplefs.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/gui/module.hpp"
#include "ok/interrupt/interrupt.hpp"
#include "ok/ipc/ipc.hpp"
#include "ok/memory/memory.hpp"
#include "ok/net/net.hpp"
#include "ok/posix/posix.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/smp/smp.hpp"
#include "ok/syscall/syscall.hpp"
#include "ok/user/user.hpp"

#include <array>

namespace ok
{

struct KernelModuleModes
{
    memory::TranslationMode memory{memory::TranslationMode::linear};
    interrupt::DispatchMode interrupts{interrupt::DispatchMode::direct};
    smp::TopologyMode smp{smp::TopologyMode::single_core};
    sched::SchedulingMode scheduler{sched::SchedulingMode::round_robin};
    ipc::DeliveryMode ipc{ipc::DeliveryMode::copy};
    syscall::DispatchMode syscalls{syscall::DispatchMode::trap};
    driver::IoMode drivers{driver::IoMode::polling};
    fs::FileSystemMode filesystem{fs::FileSystemMode::ram_only};
    user::TransitionMode user{user::TransitionMode::simulated};
};

struct KernelConfig
{
    arch::Architecture architecture{arch::Architecture::x86_64};
    std::array<memory::MemoryRegion, 8> memory_map{};
    usize memory_region_count{0};
    usize timer_hz{1000};
    KernelModuleModes modes{};
};

struct KernelTestReport
{
    bool memory{false};
    bool interrupts{false};
    bool ipc{false};
    bool vfs{false};
    bool simplefs{false};
    bool ext4{false};
    bool syscalls{false};
    bool user_mode{false};
    bool display{false};
    bool gpu{false};
    bool gui{false};
    bool input{false};
    bool posix{false};
    bool bus{false};
    bool usb{false};
    bool net{false};
    bool shell{false};
    bool modes{false};
    bool modules{false};
    bool vm{false};
    bool proc{false};
    bool elf{false};
    bool userland{false};
    bool vfs_unix{false};
    bool devfs{false};
    bool pipe{false};
    bool tty{false};
    bool linux_abi{false};
    bool linux_syscalls{false};
    bool driver_abi{false};
    bool linux_driver_shim{false};
    bool module_load{false};
    bool netdev{false};
    bool sockets{false};
    bool block{false};
    bool ext4_readonly{false};
    bool smp_roadmap{false};
    bool irq_roadmap{false};
    bool preempt{false};
    usize module_count{0};
    usize module_failed_count{0};
};

class Kernel final
{
  public:
    Kernel();

    Status boot(KernelConfig config);
    Status run_debug_test_suite();
    Status handle_gui_mouse(i32 delta_x, i32 delta_y, bool left_button);
    Status handle_gui_mouse_position(i32 x, i32 y, bool left_button);
    Status handle_gui_scroll(i32 rows);
    Status handle_gui_key(int key);
    Status tick();
    Result<sched::ProcessId> create_ui_process(std::string_view name, uptr entry, uptr stack_top,
                                               user::Credentials credentials);
    Status open_file_manager(std::string_view path, bool foreground_shell_child = false);
    Status close_file_manager();
    Status open_task_manager(bool foreground_shell_child = false, std::string_view program_name = "task-manager");
    Status close_task_manager();
    Status close_debug_gui();
    Status kill_process(sched::ProcessId pid);
    Status supervise_daemons();
    Status load_external_kernel_module(std::string_view path);
    Status request_power_action(SystemPowerAction action);

    [[nodiscard]] bool booted() const
    {
        return booted_;
    }
    [[nodiscard]] usize debug_test_points_run() const
    {
        return debug_test_points_run_;
    }
    [[nodiscard]] const KernelTestReport &test_report() const
    {
        return test_report_;
    }
    [[nodiscard]] arch::ArchOperations &arch()
    {
        return *arch_;
    }
    [[nodiscard]] interrupt::InterruptDispatcher &interrupts()
    {
        return interrupts_;
    }
    [[nodiscard]] memory::MemoryManager &memory()
    {
        return memory_;
    }
    [[nodiscard]] sched::Scheduler &scheduler()
    {
        return scheduler_;
    }
    [[nodiscard]] smp::CpuTopology &topology()
    {
        return topology_;
    }
    [[nodiscard]] ipc::IpcRouter &ipc()
    {
        return ipc_;
    }
    [[nodiscard]] syscall::Table &syscalls()
    {
        return syscalls_;
    }
    [[nodiscard]] driver::DriverManager &drivers()
    {
        return drivers_;
    }
    [[nodiscard]] driver::ConsoleDriver &console()
    {
        return console_driver_;
    }
    [[nodiscard]] driver::FramebufferDisplayDriver &display()
    {
        return display_driver_;
    }
    [[nodiscard]] driver::VirtioGpuPciDisplayDriver &virtio_gpu()
    {
        return virtio_gpu_driver_;
    }
    [[nodiscard]] gui::GuiModule &gui()
    {
        return gui_module_;
    }
    [[nodiscard]] ExternalGuiDesktopModule *loaded_gui_desktop_module()
    {
        return external_gui_desktop_module_.configured() ? &external_gui_desktop_module_ : nullptr;
    }
    [[nodiscard]] apps::KernelFileManager &file_manager()
    {
        if (active_file_manager_index_ < file_managers_.size())
        {
            return file_managers_[active_file_manager_index_];
        }
        return inactive_file_manager_;
    }
    [[nodiscard]] apps::KernelTaskManager &task_manager()
    {
        if (active_task_manager_index_ < task_managers_.size())
        {
            return task_managers_[active_task_manager_index_];
        }
        return inactive_task_manager_;
    }
    [[nodiscard]] ModuleManager &kernel_modules()
    {
        return kernel_modules_;
    }
    [[nodiscard]] driver::VirtioBlockDriver &virtio_block()
    {
        return virtio_block_driver_;
    }
    [[nodiscard]] driver::KeyboardDriver &keyboard()
    {
        return keyboard_driver_;
    }
    [[nodiscard]] driver::Ps2MouseDriver &mouse()
    {
        return mouse_driver_;
    }
    [[nodiscard]] driver::PciBusDriver &pci()
    {
        return pci_bus_driver_;
    }
    [[nodiscard]] driver::UsbXhciControllerDriver &usb()
    {
        return usb_xhci_driver_;
    }
    [[nodiscard]] driver::UsbHidKeyboardDriver &usb_keyboard()
    {
        return usb_keyboard_driver_;
    }
    [[nodiscard]] driver::UsbHidMouseDriver &usb_mouse()
    {
        return usb_mouse_driver_;
    }
    [[nodiscard]] driver::BlockDevice &disk()
    {
        return virtio_block_driver_.bound() ? static_cast<driver::BlockDevice &>(virtio_block_driver_)
                                            : static_cast<driver::BlockDevice &>(ram_block_driver_);
    }
    [[nodiscard]] std::string_view disk_name() const
    {
        return virtio_block_driver_.bound() ? virtio_block_driver_.name() : ram_block_driver_.name();
    }
    [[nodiscard]] fs::VirtualFileSystem &vfs()
    {
        return vfs_;
    }
    [[nodiscard]] fs::SimpleDiskFileSystem &simplefs()
    {
        return simplefs_;
    }
    [[nodiscard]] net::NetworkStack &network()
    {
        return network_;
    }
    [[nodiscard]] posix::PosixService &posix()
    {
        return posix_;
    }
    [[nodiscard]] user::UserSpaceManager &user_space()
    {
        return user_space_;
    }
    [[nodiscard]] KernelDebugShell &debug_shell()
    {
        return debug_shell_;
    }
    [[nodiscard]] SystemPowerAction power_action() const
    {
        return power_action_;
    }

  private:
    Status register_builtin_interrupts(driver::TimerDriver &timer);
    Status register_builtin_syscalls(posix::PosixService &posix);
    Status ensure_kernel_log_file();
    Status append_kernel_log_line(std::string_view line);
    Status log_boot_line(std::string_view line);
    Status log_daemon_restart(std::string_view kind, std::string_view process_name, sched::ProcessId previous_pid,
                              sched::ProcessId pid);
    Status run_ext4_test();
    Status handle_gui_window_event(gui::WindowEvent event);
    Status handle_gui_close_request(gui::SurfaceId surface);
    Status handle_gui_surface_changed(gui::SurfaceId surface);
    Status handle_gui_taskbar_launcher(gui::TaskbarApp app);
    Status handle_system_gui_dock_launcher(ExternalGuiDockApp app);
    Status start_selected_system_gui_session(ExternalGuiDesktopModule &desktop);
    Status focus_external_gui_app(std::string_view service_id, std::string_view path);
    Status sync_gui_credentials_from_surface(gui::SurfaceId surface);
    Status reconcile_file_managers();
    Status reconcile_task_managers();
    Status close_file_manager_at(usize index, bool kill_process, bool notify_shell);
    Status close_task_manager_at(usize index, bool kill_process, bool notify_shell);
    Status close_all_file_managers();
    Status close_all_task_managers();
    Status focus_file_manager();
    Status focus_file_manager_at(usize index);
    Status focus_task_manager();
    Status focus_task_manager_at(usize index);
    Result<fs::FileBuffer> read_external_module_file(std::string_view path);
    Status load_external_gui_desktop_module(std::string_view path, const ModuleImageInfo &image);
    Status load_external_gui_app_module(std::string_view path, const ModuleImageInfo &image);
    Status load_system_gui_app_modules();
    Status refresh_external_gui_modules(bool focus_desktop);
    Status force_close_gui_surface(gui::SurfaceId surface);
    Status note_ignored_gui_close(gui::SurfaceId surface);
    Status show_force_close_prompt(gui::SurfaceId surface);
    void clear_gui_close_attempt(gui::SurfaceId surface);
    [[nodiscard]] Result<usize> find_file_manager_by_surface(gui::SurfaceId surface) const;
    [[nodiscard]] Result<usize> find_file_manager_by_process(sched::ProcessId pid) const;
    [[nodiscard]] Result<usize> find_task_manager_by_surface(gui::SurfaceId surface) const;
    [[nodiscard]] Result<usize> find_task_manager_by_process(sched::ProcessId pid) const;

    struct GuiCloseAttempt
    {
        gui::SurfaceId surface{0};
        gui::SurfaceId prompt_surface{0};
        u8 count{0};
    };

    bool booted_{false};
    bool gui_mouse_left_down_{false};
    SystemPowerAction power_action_{SystemPowerAction::none};
    usize debug_test_points_run_{0};
    u64 kernel_tick_count_{0};
    u64 task_manager_next_refresh_tick_{0};
    KernelTestReport test_report_{};
    KernelConfig config_{};
    arch::ArchOperations *arch_{nullptr};
    driver::ConsoleDriver console_driver_{};
    driver::TimerDriver timer_driver_{};
    driver::NullBlockDriver null_block_driver_{};
    driver::RamBlockDriver ram_block_driver_{};
    driver::VirtioBlockDriver virtio_block_driver_{};
    driver::FramebufferDisplayDriver display_driver_{};
    driver::VirtioGpuPciDisplayDriver virtio_gpu_driver_{};
    gui::GuiModule gui_module_{};
    ExternalGuiDesktopModule external_gui_desktop_module_{};
    std::array<ExternalGuiAppModule, max_external_gui_apps> external_gui_app_modules_{};
    StaticVector<apps::KernelFileManager, gui::max_gui_surfaces> file_managers_{};
    apps::KernelFileManager inactive_file_manager_{};
    usize active_file_manager_index_{gui::max_gui_surfaces};
    StaticVector<apps::KernelTaskManager, gui::max_gui_surfaces> task_managers_{};
    apps::KernelTaskManager inactive_task_manager_{};
    usize active_task_manager_index_{gui::max_gui_surfaces};
    driver::KeyboardDriver keyboard_driver_{};
    driver::Ps2MouseDriver mouse_driver_{};
    driver::PciBusDriver pci_bus_driver_{};
    driver::UsbXhciControllerDriver usb_xhci_driver_{};
    driver::UsbHidKeyboardDriver usb_keyboard_driver_{};
    driver::UsbHidMouseDriver usb_mouse_driver_{};
    interrupt::InterruptDispatcher interrupts_;
    memory::MemoryManager memory_;
    sched::Scheduler scheduler_;
    smp::CpuTopology topology_;
    ipc::IpcRouter ipc_;
    syscall::Table syscalls_;
    ModuleManager kernel_modules_;
    driver::DriverManager drivers_;
    fs::VirtualFileSystem vfs_;
    fs::SimpleDiskFileSystem simplefs_;
    net::NetworkStack network_;
    posix::PosixService posix_;
    user::UserSpaceManager user_space_;
    KernelDebugShell debug_shell_;
    StaticVector<GuiCloseAttempt, gui::max_gui_surfaces> gui_close_attempts_{};
};

} // namespace ok
