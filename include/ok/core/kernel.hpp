#pragma once

#include "ok/arch/arch.hpp"
#include "ok/driver/driver.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/interrupt/interrupt.hpp"
#include "ok/ipc/ipc.hpp"
#include "ok/memory/memory.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/smp/smp.hpp"
#include "ok/syscall/syscall.hpp"
#include "ok/user/user.hpp"

#include <array>

namespace ok
{

struct KernelConfig
{
    arch::Architecture architecture{arch::Architecture::x86_64};
    std::array<memory::MemoryRegion, 8> memory_map{};
    usize memory_region_count{0};
    usize timer_hz{1000};
};

struct KernelSmokeReport
{
    bool memory{false};
    bool interrupts{false};
    bool ipc{false};
    bool vfs{false};
    bool ext4{false};
    bool syscalls{false};
    bool user_mode{false};
    bool display{false};
};

class Kernel final
{
  public:
    Kernel();

    Status boot(KernelConfig config);
    Status run_smoke_suite();

    [[nodiscard]] bool booted() const
    {
        return booted_;
    }
    [[nodiscard]] usize debug_test_points_run() const
    {
        return debug_test_points_run_;
    }
    [[nodiscard]] const KernelSmokeReport &smoke_report() const
    {
        return smoke_report_;
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
    [[nodiscard]] fs::VirtualFileSystem &vfs()
    {
        return vfs_;
    }
    [[nodiscard]] user::UserSpaceManager &user_space()
    {
        return user_space_;
    }

  private:
    Status register_builtin_interrupts(driver::TimerDriver &timer);
    Status register_builtin_syscalls(driver::ConsoleDriver &console);
    Status log_boot_line(std::string_view line);
    Status run_ext4_smoke();

    bool booted_{false};
    usize debug_test_points_run_{0};
    KernelSmokeReport smoke_report_{};
    KernelConfig config_{};
    arch::ArchOperations *arch_{nullptr};
    driver::ConsoleDriver console_driver_{};
    driver::TimerDriver timer_driver_{};
    driver::NullBlockDriver null_block_driver_{};
    driver::FramebufferDisplayDriver display_driver_{};
    interrupt::InterruptDispatcher interrupts_;
    memory::MemoryManager memory_;
    sched::Scheduler scheduler_;
    smp::CpuTopology topology_;
    ipc::IpcRouter ipc_;
    syscall::Table syscalls_;
    driver::DriverManager drivers_;
    fs::VirtualFileSystem vfs_;
    user::UserSpaceManager user_space_;
};

} // namespace ok
