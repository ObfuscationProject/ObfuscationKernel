#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/shell.hpp"
#include "ok/driver/driver.hpp"
#include "ok/fs/simplefs.hpp"
#include "ok/fs/vfs.hpp"
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
    bool input{false};
    bool posix{false};
    bool bus{false};
    bool usb{false};
    bool net{false};
    bool shell{false};
    bool modes{false};
};

class Kernel final
{
  public:
    Kernel();

    Status boot(KernelConfig config);
    Status run_debug_test_suite();

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
    [[nodiscard]] driver::RamBlockDriver &disk()
    {
        return ram_block_driver_;
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

  private:
    Status register_builtin_interrupts(driver::TimerDriver &timer);
    Status register_builtin_syscalls(posix::PosixService &posix);
    Status log_boot_line(std::string_view line);
    Status run_ext4_test();

    bool booted_{false};
    usize debug_test_points_run_{0};
    KernelTestReport test_report_{};
    KernelConfig config_{};
    arch::ArchOperations *arch_{nullptr};
    driver::ConsoleDriver console_driver_{};
    driver::TimerDriver timer_driver_{};
    driver::NullBlockDriver null_block_driver_{};
    driver::RamBlockDriver ram_block_driver_{};
    driver::FramebufferDisplayDriver display_driver_{};
    driver::VirtioGpuPciDisplayDriver virtio_gpu_driver_{};
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
    driver::DriverManager drivers_;
    fs::VirtualFileSystem vfs_;
    fs::SimpleDiskFileSystem simplefs_;
    net::NetworkStack network_;
    posix::PosixService posix_;
    user::UserSpaceManager user_space_;
    KernelDebugShell debug_shell_;
};

} // namespace ok
