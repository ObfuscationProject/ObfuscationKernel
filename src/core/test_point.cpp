#include "ok/core/test_point.hpp"

#include "ok/core/kernel.hpp"
#include "ok/fs/ext4.hpp"

#if defined(OK_ENABLE_TEST_POINTS)
#include <array>
#include <span>
#endif

namespace ok::test {

#if !defined(OK_ENABLE_TEST_POINTS)

Result<usize> run_kernel_test_points(Kernel&)
{
    return usize {0};
}

#else
namespace {

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

class TestPointRunner final {
public:
    Status check(std::string_view, std::string_view, bool condition, std::string_view failure)
    {
        ++count_;
        if (!condition) {
            return Status::fault(failure);
        }
        return Status::success();
    }

    Status check_status(std::string_view, std::string_view, Status status)
    {
        ++count_;
        return status;
    }

    [[nodiscard]] usize count() const { return count_; }

private:
    usize count_ {0};
};

Status test_architecture_profiles(TestPointRunner& runner)
{
    const std::array architectures {
        arch::configured_architecture(),
    };

    for (auto architecture : architectures) {
        auto& operations = arch::arch_operations(architecture);
        const auto module = arch::to_string(architecture);
        if (auto status = runner.check(module,
                                       "identity",
                                       operations.architecture() == architecture,
                                       "architecture identity mismatch");
            !status.ok()) {
            return status;
        }
        if (auto status = runner.check(module, "page-size", operations.page_size() >= 4096, "invalid page size");
            !status.ok()) {
            return status;
        }
        if (auto status = runner.check(module, "register-count", operations.register_count() > 0, "invalid register count");
            !status.ok()) {
            return status;
        }
        if (auto status =
                runner.check(module, "interrupt-model", !operations.interrupt_model().empty(), "missing interrupt model");
            !status.ok()) {
            return status;
        }
        if (auto status = runner.check(module, "syscall-model", !operations.syscall_model().empty(), "missing syscall model");
            !status.ok()) {
            return status;
        }
        if (auto status = runner.check(module,
                                       "user-transition-model",
                                       !operations.user_transition_model().empty(),
                                       "missing user transition model");
            !status.ok()) {
            return status;
        }

        auto kernel_context = operations.make_kernel_context(0x1000, 0x8000);
        if (auto status = runner.check(module,
                                       "kernel-context",
                                       kernel_context.mode == arch::PrivilegeMode::kernel &&
                                           kernel_context.program_counter == 0x1000,
                                       "invalid kernel context");
            !status.ok()) {
            return status;
        }

        auto user_context = operations.make_user_context(arch::UserEntry {
            .instruction_pointer = 0x400000,
            .stack_pointer = 0x800000,
            .argument = 0x55,
        });
        if (auto status = runner.check(module,
                                       "user-context",
                                       user_context.mode == arch::PrivilegeMode::user &&
                                           user_context.registers[0] == 0x55,
                                       "invalid user context");
            !status.ok()) {
            return status;
        }

        operations.memory_fence();
        if (auto status =
                runner.check(module, "cycle-counter", operations.read_cycle_counter() != 0, "cycle counter returned zero");
            !status.ok()) {
            return status;
        }
        operations.disable_interrupts();
        operations.enable_interrupts();
        operations.halt();
        if (auto status = runner.check(module, "control-ops", true, "architecture control operations failed"); !status.ok()) {
            return status;
        }
    }

    return Status::success();
}

Status test_interrupts(TestPointRunner& runner)
{
    interrupt::InterruptDispatcher dispatcher;
    if (auto status = runner.check_status("interrupt", "register", dispatcher.register_callback(90, "debug-test", nullptr, [](void*, arch::TrapFrame&) {
            return Status::success();
        }));
        !status.ok()) {
        return status;
    }
    arch::TrapFrame frame {.vector = 90};
    if (auto status = runner.check_status("interrupt", "dispatch", dispatcher.dispatch(frame)); !status.ok()) {
        return status;
    }
    return runner.check("interrupt", "count", dispatcher.handled_count(90) == 1, "interrupt dispatch count mismatch");
}

Status test_memory(TestPointRunner& runner)
{
    memory::MemoryManager manager;
    const std::array regions {
        memory::MemoryRegion {.base = 0x1000, .size = 0x10000, .type = memory::RegionType::usable},
    };
    if (auto status = runner.check_status("memory", "initialize", manager.initialize(regions, 4096)); !status.ok()) {
        return status;
    }
    auto frame = manager.frames().allocate();
    if (auto status = runner.check("memory", "allocate", frame.ok(), "frame allocation failed"); !status.ok()) {
        return status;
    }
    if (auto status = runner.check_status("memory", "map", manager.kernel_address_space().map(0x2000, frame.value(), 0b11));
        !status.ok()) {
        return status;
    }
    if (auto status = runner.check_status("memory", "unmap", manager.kernel_address_space().unmap(0x2000)); !status.ok()) {
        return status;
    }
    return runner.check_status("memory", "release", manager.frames().release(frame.value()));
}

Status test_scheduler(TestPointRunner& runner)
{
    sched::Scheduler scheduler;
    if (auto status = runner.check_status("sched", "configure-cpus", scheduler.configure_cpus(2)); !status.ok()) {
        return status;
    }
    auto& operations = arch::arch_operations(arch::Architecture::x86_64);
    auto first = scheduler.create_process("first", operations.make_kernel_context(0x1000, 0x8000));
    auto second = scheduler.create_process("second", operations.make_kernel_context(0x2000, 0x9000));
    if (auto status = runner.check("sched", "create", first.ok() && second.ok(), "process creation failed"); !status.ok()) {
        return status;
    }
    if (auto status = runner.check_status("sched", "runnable-first", scheduler.set_runnable(first.value())); !status.ok()) {
        return status;
    }
    if (auto status = runner.check_status("sched", "runnable-second", scheduler.set_runnable(second.value())); !status.ok()) {
        return status;
    }
    auto selected = scheduler.schedule_next();
    if (auto status =
            runner.check("sched", "schedule", selected.ok() && selected.value() != 0, "scheduler did not select a process");
        !status.ok()) {
        return status;
    }
    auto selected_on_cpu = scheduler.schedule_next_on_cpu(1);
    return runner.check("sched",
                        "schedule-cpu",
                        selected_on_cpu.ok() && scheduler.current_pid(1) == selected_on_cpu.value(),
                        "scheduler did not track per-cpu process state");
}

Status test_smp(TestPointRunner& runner)
{
    smp::CpuTopology topology;
    if (auto status = runner.check_status("smp", "initialize", topology.initialize(4)); !status.ok()) {
        return status;
    }
    for (usize cpu = 1; cpu < 4; ++cpu) {
        const auto cpu_id = static_cast<smp::CpuId>(cpu);
        if (auto status = runner.check_status("smp", "starting", topology.mark_starting(cpu_id)); !status.ok()) {
            return status;
        }
        if (auto status = runner.check_status("smp", "online", topology.mark_online(cpu_id)); !status.ok()) {
            return status;
        }
    }
    if (auto status = runner.check("smp", "online-count", topology.online_count() == 4, "SMP online count mismatch");
        !status.ok()) {
        return status;
    }
    if (auto status = runner.check_status("smp", "record-schedule", topology.record_schedule(2)); !status.ok()) {
        return status;
    }
    auto least_busy = topology.least_busy_online_cpu();
    if (auto status = runner.check("smp", "least-busy", least_busy.ok(), "least busy CPU selection failed"); !status.ok()) {
        return status;
    }
    smp::SpinLock lock;
    if (auto status = runner.check("smp", "try-lock", lock.try_lock(), "spinlock try_lock failed"); !status.ok()) {
        return status;
    }
    lock.unlock();
    return Status::success();
}

Status test_ipc(TestPointRunner& runner)
{
    ipc::IpcRouter router;
    auto channel = router.create_channel(1);
    if (auto status = runner.check("ipc", "create-channel", channel.ok(), "channel creation failed"); !status.ok()) {
        return status;
    }
    struct Payload {
        u32 value;
    };
    if (auto status = runner.check_status("ipc", "send", router.send_value(channel.value(), 1, 2, Payload {7}));
        !status.ok()) {
        return status;
    }
    auto message = router.receive(channel.value());
    return runner.check("ipc", "receive", message.ok() && message.value().size == sizeof(Payload), "IPC receive failed");
}

Status test_syscalls(TestPointRunner& runner)
{
    syscall::Table table;
    if (auto status = runner.check_status("syscall", "register", table.register_callback(syscall::Number::ok_debug,
                                                                                          "debug",
                                                                                          nullptr,
                                                                                          [](void*, const syscall::Request& request) {
                                                                                              return syscall::Response {
                                                                                                  .value = static_cast<i64>(
                                                                                                      request.args[0]),
                                                                                                  .status = Status::success()};
                                                                                          }));
        !status.ok()) {
        return status;
    }
    auto response = table.dispatch(syscall::Request {.number = syscall::Number::ok_debug, .args = {11, 0, 0, 0, 0, 0}});
    return runner.check("syscall", "dispatch", response.status.ok() && response.value == 11, "syscall dispatch failed");
}

Status test_drivers(TestPointRunner& runner)
{
    driver::DriverManager manager;
    driver::ConsoleDriver console;
    driver::TimerDriver timer;
    driver::NullBlockDriver block;
    driver::FramebufferDisplayDriver display;
    static_cast<void>(manager.add(console));
    static_cast<void>(manager.add(timer));
    static_cast<void>(manager.add(block));
    static_cast<void>(manager.add(display));
    if (auto status = runner.check_status("driver", "start-all", manager.start_all()); !status.ok()) {
        return status;
    }
    if (auto status = runner.check_status("driver", "console-write", console.write("debug")); !status.ok()) {
        return status;
    }
    if (auto status = runner.check_status("driver", "display-fill", display.fill_rect(2, 2, 8, 8, 0xff3366aau));
        !status.ok()) {
        return status;
    }
    if (auto status = runner.check("driver", "display-checksum", display.checksum() != 0, "display checksum failed");
        !status.ok()) {
        return status;
    }
    return runner.check("driver",
                        "find",
                        manager.find(driver::Class::timer) != nullptr && manager.find(driver::Class::display) != nullptr,
                        "driver lookup failed");
}

Status test_filesystem(TestPointRunner& runner)
{
    fs::VirtualFileSystem vfs;
    if (auto status = runner.check_status("fs", "create", vfs.create("/tmp/debug.txt", fs::NodeType::regular)); !status.ok()) {
        return status;
    }
    constexpr std::string_view text {"debug"};
    std::span<const std::byte> bytes {reinterpret_cast<const std::byte*>(text.data()), text.size()};
    if (auto status = runner.check_status("fs", "write", vfs.write_file("/tmp/debug.txt", bytes)); !status.ok()) {
        return status;
    }
    auto read = vfs.read_file("/tmp/debug.txt");
    return runner.check("fs", "read", read.ok() && read.value().size == text.size(), "VFS read failed");
}

Status test_ext4(TestPointRunner& runner)
{
    std::array<std::byte, 4096> image {};
    auto bytes = std::span<std::byte>(image.data(), image.size());
    const auto base = fs::ext4_superblock_offset;
    write_le32(bytes, base + 0x00, 128);
    write_le32(bytes, base + 0x04, 4);
    write_le32(bytes, base + 0x0c, 2);
    write_le32(bytes, base + 0x18, 0);
    write_le16(bytes, base + 0x38, fs::ext4_superblock_magic);
    write_le16(bytes, base + 0x58, 256);
    write_le32(bytes, base + 0x60, 0x40);
    constexpr std::string_view name {"OKEXT4"};
    for (usize i = 0; i < name.size(); ++i) {
        bytes[base + 0x78 + i] = static_cast<std::byte>(name[i]);
    }

    fs::Ext4Volume volume;
    if (auto status = runner.check_status("ext4", "mount", volume.mount(image)); !status.ok()) {
        return status;
    }
    auto info = volume.info();
    if (auto status = runner.check("ext4",
                                   "superblock",
                                   info.ok() && info.value().block_size == 1024 && info.value().inode_size == 256 &&
                                       info.value().volume_name.view() == name && info.value().has_extents,
                                   "ext4 superblock parse failed");
        !status.ok()) {
        return status;
    }
    std::array<std::byte, 1024> block {};
    return runner.check_status("ext4", "read-block", volume.read_block(1, block));
}

Status test_user_mode(TestPointRunner& runner)
{
    user::UserSpaceManager manager;
    auto& operations = arch::arch_operations(arch::Architecture::aarch64);
    auto context = operations.make_kernel_context(0x1000, 0x8000);
    auto status = manager.enter_process(1,
                                        arch::UserEntry {
                                            .instruction_pointer = 0x400000,
                                            .stack_pointer = 0x800000,
                                            .argument = 3,
                                        },
                                        context);
    if (auto checked = runner.check_status("user", "enter", status); !checked.ok()) {
        return checked;
    }
    return runner.check("user", "mode", context.mode == arch::PrivilegeMode::user, "user mode transition failed");
}

} // namespace

Result<usize> run_kernel_test_points(Kernel&)
{
    TestPointRunner runner;

    if (auto status = test_architecture_profiles(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_interrupts(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_memory(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_scheduler(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_smp(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_ipc(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_syscalls(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_drivers(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_filesystem(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_ext4(runner); !status.ok()) {
        return status;
    }
    if (auto status = test_user_mode(runner); !status.ok()) {
        return status;
    }

    return runner.count();
}

#endif

} // namespace ok::test
