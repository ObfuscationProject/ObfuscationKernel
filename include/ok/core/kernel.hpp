#pragma once

#include "ok/arch/arch.hpp"
#include "ok/driver/driver.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/interrupt/interrupt.hpp"
#include "ok/ipc/ipc.hpp"
#include "ok/memory/memory.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/syscall/syscall.hpp"
#include "ok/user/user.hpp"

#include <memory>
#include <vector>

namespace ok {

struct KernelConfig {
    arch::Architecture architecture {arch::Architecture::host};
    std::vector<memory::MemoryRegion> memory_map {};
    usize timer_hz {1000};
};

class Kernel final {
public:
    Kernel();

    Status boot(KernelConfig config);
    Status run_smoke_suite();

    [[nodiscard]] bool booted() const { return booted_; }
    [[nodiscard]] arch::ArchOperations& arch() { return *arch_; }
    [[nodiscard]] interrupt::InterruptDispatcher& interrupts() { return interrupts_; }
    [[nodiscard]] memory::MemoryManager& memory() { return memory_; }
    [[nodiscard]] sched::Scheduler& scheduler() { return scheduler_; }
    [[nodiscard]] ipc::IpcRouter& ipc() { return ipc_; }
    [[nodiscard]] syscall::Table& syscalls() { return syscalls_; }
    [[nodiscard]] driver::DriverManager& drivers() { return drivers_; }
    [[nodiscard]] fs::VirtualFileSystem& vfs() { return vfs_; }
    [[nodiscard]] user::UserSpaceManager& user_space() { return user_space_; }

private:
    Status register_builtin_interrupts(driver::TimerDriver& timer);
    Status register_builtin_syscalls(driver::ConsoleDriver& console);

    bool booted_ {false};
    KernelConfig config_ {};
    std::unique_ptr<arch::ArchOperations> arch_;
    interrupt::InterruptDispatcher interrupts_;
    memory::MemoryManager memory_;
    sched::Scheduler scheduler_;
    ipc::IpcRouter ipc_;
    syscall::Table syscalls_;
    driver::DriverManager drivers_;
    fs::VirtualFileSystem vfs_;
    user::UserSpaceManager user_space_;
};

} // namespace ok

