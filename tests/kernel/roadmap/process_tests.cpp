#include "roadmap_tests.hpp"

#include "ok/posix/posix.hpp"
#include "ok/sched/process.hpp"
#include "ok/syscall/syscall.hpp"
#include "ok/user/user.hpp"

#include <array>
#include <span>

namespace ok
{
namespace
{

std::span<const std::byte> process_test_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

bool contains_text(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (needle.size() > haystack.size())
    {
        return false;
    }
    for (usize offset = 0; offset <= haystack.size() - needle.size(); ++offset)
    {
        bool matched = true;
        for (usize i = 0; i < needle.size(); ++i)
        {
            if (haystack[offset + i] != needle[i])
            {
                matched = false;
                break;
            }
        }
        if (matched)
        {
            return true;
        }
    }
    return false;
}

Status append_unsigned(FixedString<32> &out, u64 value)
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

u16 elf_machine_for(arch::Architecture architecture)
{
    switch (architecture)
    {
    case arch::Architecture::i386:
        return 0x03;
    case arch::Architecture::aarch64:
        return 0xb7;
    case arch::Architecture::arm32:
        return 0x28;
    case arch::Architecture::rv64:
    case arch::Architecture::rv32:
        return 0xf3;
    case arch::Architecture::mips:
    case arch::Architecture::mips64:
        return 0x08;
    case arch::Architecture::ppc:
        return 0x14;
    case arch::Architecture::loongarch64:
        return 0x102;
    case arch::Architecture::x86_64:
        return 0x3e;
    }
    return 0x3e;
}

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
    write_le16(bytes, 18, elf_machine_for(architecture));
    write_le64(bytes, 24, 0x400000);
    write_le64(bytes, 32, 64);
    write_le16(bytes, 54, 56);
    write_le16(bytes, 56, 1);
    return image;
}

arch::Architecture different_architecture(arch::Architecture architecture)
{
    return architecture == arch::Architecture::x86_64 ? arch::Architecture::aarch64 : arch::Architecture::x86_64;
}

Status verify_scheduler_runs_two(arch::Architecture architecture)
{
    sched::Scheduler scheduler;
    if (auto status = scheduler.configure_cpus(1); !status.ok())
    {
        return status;
    }
    auto &ops = arch::arch_operations(architecture);
    auto first = scheduler.create_process("sched-a", ops.make_kernel_context(0x1000, 0x8000));
    auto second = scheduler.create_process("sched-b", ops.make_kernel_context(0x2000, 0x9000));
    if (!first || !second)
    {
        return Status::fault("scheduler process creation failed");
    }
    if (auto status = scheduler.set_runnable(first.value()); !status.ok())
    {
        return status;
    }
    if (auto status = scheduler.set_runnable(second.value()); !status.ok())
    {
        return status;
    }
    auto first_pick = scheduler.schedule_next();
    auto second_pick = scheduler.schedule_next();
    if (!first_pick || !second_pick || first_pick.value() == second_pick.value())
    {
        return Status::fault("scheduler did not run both test processes");
    }
    return Status::success();
}

Status verify_elf_loader(arch::Architecture architecture, std::span<const std::byte> elf)
{
    sched::ElfLoader loader;
    auto loaded = loader.load(elf, architecture);
    if (!loaded || loaded.value().entry != 0x400000 || loaded.value().stack_pointer == 0 ||
        loaded.value().load_segments != 1)
    {
        return Status::fault("ELF loader did not expose entry, stack, and segment metadata");
    }

    auto bad_magic = make_test_elf(architecture);
    bad_magic[0] = std::byte{0};
    if (loader.load(bad_magic, architecture).status().code() != StatusCode::invalid_argument)
    {
        return Status::fault("ELF loader accepted invalid magic");
    }

    auto wrong_arch = make_test_elf(different_architecture(architecture));
    if (loader.load(wrong_arch, architecture).status().code() != StatusCode::invalid_argument)
    {
        return Status::fault("ELF loader accepted an architecture mismatch");
    }

    auto no_program_headers = make_test_elf(architecture);
    write_le16(std::span<std::byte>(no_program_headers.data(), no_program_headers.size()), 56, 0);
    if (loader.load(no_program_headers, architecture).status().code() != StatusCode::invalid_argument)
    {
        return Status::fault("ELF loader accepted an image without program headers");
    }
    return Status::success();
}

Status verify_wait_queue()
{
    sched::WaitQueue queue;
    if (auto status = queue.block(42); !status.ok())
    {
        return status;
    }
    if (!queue.blocked(42) || queue.block(42).code() != StatusCode::already_exists)
    {
        return Status::fault("wait queue did not track blocked process state");
    }
    if (auto status = queue.wake(42); !status.ok())
    {
        return status;
    }
    if (queue.blocked(42) || queue.wake(42).code() != StatusCode::not_found)
    {
        return Status::fault("wait queue did not wake blocked process state");
    }
    return Status::success();
}

Status verify_process_lifecycle(Kernel &kernel, std::span<const std::byte> elf, sched::ProcessManager &processes)
{
    auto &ops = arch::arch_operations(kernel.arch().architecture());

    auto first = processes.create_kernel_thread("kthread-a", ops.make_kernel_context(0x1000, 0x8000));
    auto second = processes.create_kernel_thread("kthread-b", ops.make_kernel_context(0x2000, 0x9000));
    if (!first || !second || processes.process_count() != 2)
    {
        return Status::fault("kernel thread creation failed");
    }

    auto reparented = processes.fork(second.value());
    if (!reparented)
    {
        return reparented.status();
    }
    if (auto status = processes.reparent_children(second.value(), first.value()); !status.ok())
    {
        return status;
    }
    const auto *reparented_process = processes.find(reparented.value());
    if (reparented_process == nullptr || reparented_process->parent_pid() != first.value())
    {
        return Status::fault("process reparenting to init-style process failed");
    }
    if (auto status = processes.exit(reparented.value(), 0); !status.ok())
    {
        return status;
    }
    auto reparent_wait = processes.wait4(first.value(), reparented.value());
    if (!reparent_wait || reparent_wait.value() != 0)
    {
        return Status::fault("wait4 failed for reparented child");
    }

    auto user = processes.create_user_process("hello", elf, kernel.arch().architecture());
    if (!user)
    {
        return user.status();
    }
    auto *user_process = processes.find(user.value());
    if (user_process == nullptr || user_process->memory_map().area_count() == 0 || user_process->threads().empty() ||
        user_process->threads()[0].context.mode != arch::PrivilegeMode::user)
    {
        return Status::fault("user process ELF load failed");
    }
    if (auto status = user_process->fd_table().open_slot(7); !status.ok())
    {
        return status;
    }
    if (auto status = user_process->memory_map().add_area(memory::VmArea{
            .base = 0x600000,
            .length = 0x1000,
            .permissions = memory::page_read | memory::page_write | memory::page_user,
        });
        !status.ok())
    {
        return status;
    }

    auto child = processes.fork(user.value());
    if (!child)
    {
        return child.status();
    }
    const auto *child_process = processes.find(child.value());
    if (child_process == nullptr || !child_process->fd_table().contains(7) ||
        child_process->fd_table().open_count() != user_process->fd_table().open_count() ||
        child_process->memory_map().area_count() != user_process->memory_map().area_count())
    {
        return Status::fault("fork did not duplicate FD table and memory map metadata");
    }

    const auto fd_count_before_exec = child_process->fd_table().open_count();
    if (auto status = processes.execve(child.value(), elf, kernel.arch().architecture()); !status.ok())
    {
        return status;
    }
    auto *execed_child = processes.find(child.value());
    if (execed_child == nullptr || execed_child->memory_map().area_count() != 1 ||
        execed_child->fd_table().open_count() != fd_count_before_exec || !execed_child->fd_table().contains(7) ||
        execed_child->parent_pid() != user.value() ||
        execed_child->threads()[0].context.mode != arch::PrivilegeMode::user)
    {
        return Status::fault("execve did not replace address space while preserving selected metadata");
    }

    auto extra_thread = processes.create_user_thread(child.value(), ops.make_user_context(arch::UserEntry{
                                                                        .instruction_pointer = 0x401000,
                                                                        .stack_pointer = 0x810000,
                                                                        .argument = 1,
                                                                    }));
    if (!extra_thread)
    {
        return extra_thread.status();
    }
    if (auto status = processes.exit_group(child.value(), 17); !status.ok())
    {
        return status;
    }
    auto *exited_child = processes.find(child.value());
    if (exited_child == nullptr || exited_child->state() != sched::TaskState::zombie)
    {
        return Status::fault("exit_group did not mark process zombie");
    }
    for (const auto &thread : exited_child->threads())
    {
        if (thread.state != sched::TaskState::exited)
        {
            return Status::fault("exit_group did not exit all process threads");
        }
    }
    auto waited = processes.wait4(user.value(), child.value());
    if (!waited || waited.value() != 17 || processes.zombie_count() != 0 || processes.find(child.value()) != nullptr)
    {
        return Status::fault("wait4 or zombie cleanup failed");
    }

    syscall::Request exit_request{.number = syscall::Number::exit, .caller = user.value(), .args = {0, 0, 0, 0, 0, 0}};
    auto exit_response = kernel.syscalls().dispatch(exit_request);
    if (!exit_response.status.ok())
    {
        return Status::fault("user process syscall exit path failed");
    }
    return Status::success();
}

Status verify_userland_smoke(Kernel &kernel, std::span<const std::byte> elf, sched::ProcessManager &processes)
{
    constexpr std::string_view hello{"hello\n"};
    auto hello_write = kernel.posix().write(1, process_test_bytes(hello));
    if (!hello_write || hello_write.value() != hello.size())
    {
        return Status::fault("userland hello smoke write failed");
    }

    static_cast<void>(kernel.posix().unlink("/tmp/process_fd.txt"));
    auto fd = kernel.posix().open("/tmp/process_fd.txt", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC, 0644);
    if (!fd)
    {
        return fd.status();
    }
    constexpr std::string_view payload{"fd-smoke"};
    auto written = kernel.posix().write(fd.value(), process_test_bytes(payload));
    if (!written || written.value() != payload.size())
    {
        return Status::fault("userland fd smoke write failed");
    }
    auto seek = kernel.posix().seek(fd.value(), 0, posix::SeekWhence::set);
    if (!seek)
    {
        return seek.status();
    }
    std::array<std::byte, payload.size()> out{};
    auto read = kernel.posix().read(fd.value(), out);
    if (!read || read.value() != payload.size())
    {
        return Status::fault("userland fd smoke read failed");
    }
    if (auto status = kernel.posix().close(fd.value()); !status.ok())
    {
        return status;
    }

    auto exec_smoke = processes.create_user_process("exec-smoke", elf, kernel.arch().architecture());
    if (!exec_smoke)
    {
        return exec_smoke.status();
    }
    if (auto status = processes.execve(exec_smoke.value(), elf, kernel.arch().architecture()); !status.ok())
    {
        return status;
    }
    auto fork_smoke = processes.fork(exec_smoke.value());
    if (!fork_smoke)
    {
        return fork_smoke.status();
    }
    if (auto status = processes.exit(fork_smoke.value(), 0); !status.ok())
    {
        return status;
    }
    auto waited = processes.wait4(exec_smoke.value(), fork_smoke.value());
    if (!waited || waited.value() != 0)
    {
        return Status::fault("userland fork/wait smoke failed");
    }
    return Status::success();
}

Status verify_user_management_and_isolation(Kernel &kernel, std::span<const std::byte> elf,
                                            sched::ProcessManager &processes)
{
    auto &users = kernel.user_space().users();
    if (users.user_count() < 3 || users.find_by_name("kernel") == nullptr || users.find_by_name("root") == nullptr ||
        users.find_by_name("user") == nullptr)
    {
        return Status::fault("default user accounts are missing");
    }

    auto root = users.credentials_for("root");
    auto normal = users.credentials_for("user");
    if (!root || !normal || !users.can_switch(user::kernel_credentials(), normal.value()) ||
        users.can_switch(normal.value(), root.value()))
    {
        return Status::fault("user credential switching policy failed");
    }
    if (auto status = users.add_user(2000, 2000, "daemon", "/srv/daemon");
        !status.ok() && status.code() != StatusCode::already_exists)
    {
        return status;
    }
    auto daemon = users.credentials_for("daemon");
    if (!daemon)
    {
        return daemon.status();
    }

    auto process = processes.create_user_process("daemon", elf, kernel.arch().architecture());
    if (!process)
    {
        return process.status();
    }
    if (auto status = processes.set_credentials(process.value(), daemon.value()); !status.ok())
    {
        return status;
    }
    auto *daemon_process = processes.find(process.value());
    if (daemon_process == nullptr || daemon_process->credentials().uid != 2000 ||
        daemon_process->credentials().euid != 2000)
    {
        return Status::fault("process credentials were not applied");
    }

    auto child = processes.fork(process.value());
    if (!child)
    {
        return child.status();
    }
    const auto *child_process = processes.find(child.value());
    if (child_process == nullptr || child_process->credentials().uid != daemon_process->credentials().uid)
    {
        return Status::fault("fork did not inherit process credentials");
    }
    const auto child_area_count = child_process->memory_map().area_count();

    if (auto status = daemon_process->memory_map().add_area(memory::VmArea{
            .base = 0x700000,
            .length = 0x1000,
            .permissions = memory::page_read | memory::page_write | memory::page_user,
        });
        !status.ok())
    {
        return status;
    }
    if (!daemon_process->memory_map().allows_user_access(0x700100, 16, memory::page_read) ||
        !daemon_process->memory_map().allows_user_access(0x700100, 16, memory::page_write))
    {
        return Status::fault("user process memory permissions were not honored");
    }
    if (daemon_process->memory_map()
            .add_area(memory::VmArea{
                .base = 0x700800,
                .length = 0x1000,
                .permissions = memory::page_read | memory::page_user,
            })
            .code() != StatusCode::already_exists)
    {
        return Status::fault("overlapping process memory area was not rejected");
    }
    if (auto status = daemon_process->memory_map().add_area(memory::VmArea{
            .base = 0x900000,
            .length = 0x1000,
            .permissions = memory::page_read,
        });
        !status.ok())
    {
        return status;
    }
    if (daemon_process->memory_map().require_user_access(0x900000, 1, memory::page_read).code() != StatusCode::denied)
    {
        return Status::fault("kernel-only process mapping allowed user access");
    }
    child_process = processes.find(child.value());
    if (child_process == nullptr || child_process->memory_map().area_count() != child_area_count)
    {
        return Status::fault("forked child memory map was not isolated from parent edits");
    }
    return Status::success();
}

Status verify_background_programs_and_posix(Kernel &kernel)
{
    auto &ops = arch::arch_operations(kernel.arch().architecture());
    auto first = kernel.scheduler().create_background_process("bg-a", ops.make_kernel_context(0x3000, 0xa000));
    auto second = kernel.scheduler().create_background_process("bg-b", ops.make_kernel_context(0x4000, 0xb000));
    auto third = kernel.scheduler().create_background_process("bg-c", ops.make_kernel_context(0x5000, 0xc000));
    if (!first || !second || !third || kernel.scheduler().background_process_count() < 3)
    {
        return Status::fault("background process creation failed");
    }

    bool saw_first = false;
    bool saw_second = false;
    bool saw_third = false;
    const auto iterations = kernel.scheduler().process_count() * 2;
    for (usize i = 0; i < iterations; ++i)
    {
        auto selected = kernel.scheduler().schedule_next();
        if (!selected)
        {
            return selected.status();
        }
        saw_first = saw_first || selected.value() == first.value();
        saw_second = saw_second || selected.value() == second.value();
        saw_third = saw_third || selected.value() == third.value();
    }
    if (!saw_first || !saw_second || !saw_third)
    {
        return Status::fault("background processes were not scheduled");
    }

    auto ps = kernel.debug_shell().execute("ps aux");
    if (!ps || !contains_text(ps.value(), "  PID USER    TTY   STAT THR COMMAND") ||
        !contains_text(ps.value(), "idle") || !contains_text(ps.value(), "mod:kernel-gui") ||
        !contains_text(ps.value(), "drv:simple-framebuffer") || !contains_text(ps.value(), "drv:ram-block0") ||
        !contains_text(ps.value(), "bg-a") ||
        !contains_text(ps.value(), "bg-b") || !contains_text(ps.value(), "bg-c"))
    {
        return Status::fault("debug shell ps did not list scheduler processes");
    }

    FixedString<32> kill_command;
    if (auto status = kill_command.assign("kill "); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kill_command, first.value()); !status.ok())
    {
        return status;
    }
    auto killed = kernel.debug_shell().execute(kill_command.view());
    const auto *killed_process = kernel.scheduler().find(first.value());
    if (!killed || !killed.value().empty() || killed_process != nullptr)
    {
        return Status::fault("debug shell kill did not remove a background process");
    }

    auto driver_pid = kernel.drivers().kernel_process_id("ram-block0");
    if (!driver_pid)
    {
        return driver_pid.status();
    }
    kill_command.clear();
    if (auto status = kill_command.assign("kill "); !status.ok())
    {
        return status;
    }
    if (auto status = append_unsigned(kill_command, driver_pid.value()); !status.ok())
    {
        return status;
    }
    auto killed_driver = kernel.debug_shell().execute(kill_command.view());
    auto restarted_driver_pid = kernel.drivers().kernel_process_id("ram-block0");
    const auto *restarted_driver = restarted_driver_pid ? kernel.scheduler().find(restarted_driver_pid.value()) : nullptr;
    if (!killed_driver || !killed_driver.value().empty() || kernel.scheduler().find(driver_pid.value()) != nullptr ||
        !restarted_driver_pid || restarted_driver_pid.value() == driver_pid.value() || restarted_driver == nullptr ||
        restarted_driver->name() != "drv:ram-block0" ||
        !contains_text(kernel.console().buffer(), "driver: restarted drv:ram-block0"))
    {
        return Status::fault("kernel user kill did not restart and log a driver daemon");
    }

    static_cast<void>(kernel.debug_shell().execute("su root"));
    auto user_ps = kernel.debug_shell().execute("ps aux");
    if (!user_ps || contains_text(user_ps.value(), "drv:") || contains_text(user_ps.value(), "mod:kernel-gui") ||
        contains_text(user_ps.value(), "idle"))
    {
        return Status::fault("debug shell ps exposed kernel processes outside kernel user");
    }
    static_cast<void>(kernel.debug_shell().execute("su user"));
    auto user_fm = kernel.debug_shell().execute("fm /tmp");
    if (!user_fm)
    {
        return Status::fault("GUI file manager command failed for active user");
    }
    if (!contains_text(user_fm.value(), "file manager: /tmp"))
    {
        return Status::fault("GUI file manager command did not report /tmp");
    }
    const auto user_fm_pid = kernel.file_manager().process_id();
    const auto *user_fm_process = kernel.scheduler().find(user_fm_pid);
    if (user_fm_pid == 0 || user_fm_process == nullptr || user_fm_process->name() != "fm:user" ||
        user_fm_process->execution() != sched::ProcessExecution::user_process ||
        user_fm_process->address_space_id() == 0)
    {
        return Status::fault("GUI file manager process did not run as isolated active user");
    }
    auto blocked_shell = kernel.debug_shell().execute("ps aux");
    if (blocked_shell || blocked_shell.status().code() != StatusCode::would_block)
    {
        return Status::fault("debug shell did not block while foreground file manager was running");
    }
    if (auto status = kernel.debug_shell().interrupt_foreground_process(); !status.ok())
    {
        return status;
    }
    if (kernel.file_manager().process_id() != 0 || kernel.scheduler().find(user_fm_pid) != nullptr)
    {
        return Status::fault("debug shell interrupt did not stop the foreground file manager");
    }
    auto resumed_shell = kernel.debug_shell().execute("echo shell-resumed");
    if (!resumed_shell || resumed_shell.value() != "shell-resumed\n")
    {
        return Status::fault("debug shell did not resume after foreground file manager exited");
    }
    static_cast<void>(kernel.debug_shell().execute("exit"));
    static_cast<void>(kernel.debug_shell().execute("su kernel"));

    const auto saved_credentials = kernel.posix().user_credentials();
    if (auto status = kernel.posix().set_credentials(user::root_credentials()); !status.ok())
    {
        return status;
    }
    static_cast<void>(kernel.posix().unlink("/tmp/background-posix.txt"));
    auto fd = kernel.posix().open("/tmp/background-posix.txt", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC, 0644);
    if (!fd)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return fd.status();
    }
    constexpr std::string_view payload{"background-posix"};
    auto written = kernel.posix().write(fd.value(), process_test_bytes(payload));
    if (!written || written.value() != payload.size())
    {
        static_cast<void>(kernel.posix().close(fd.value()));
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("background POSIX write failed");
    }
    if (auto seek = kernel.posix().seek(fd.value(), 0, posix::SeekWhence::set); !seek)
    {
        static_cast<void>(kernel.posix().close(fd.value()));
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return seek.status();
    }
    std::array<std::byte, payload.size()> out{};
    auto read = kernel.posix().read(fd.value(), out);
    if (!read || read.value() != payload.size())
    {
        static_cast<void>(kernel.posix().close(fd.value()));
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("background POSIX read failed");
    }
    if (auto status = kernel.posix().close(fd.value()); !status.ok())
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return status;
    }
    auto dir = kernel.posix().open("/tmp", posix::o_RDONLY | posix::o_DIRECTORY);
    if (!dir)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return dir.status();
    }
    std::array<std::byte, 256> dirents{};
    auto entries = kernel.posix().getdents64(dir.value(), dirents);
    static_cast<void>(kernel.posix().close(dir.value()));
    if (!entries || entries.value() == 0)
    {
        static_cast<void>(kernel.posix().set_credentials(saved_credentials));
        return Status::fault("background POSIX getdents64 failed");
    }
    return kernel.posix().set_credentials(saved_credentials);
}

} // namespace

Status run_process_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    auto elf = make_test_elf(kernel.arch().architecture());
    sched::ProcessManager processes;

    if (auto status = verify_scheduler_runs_two(kernel.arch().architecture()); !status.ok())
    {
        return status;
    }
    if (auto status = verify_elf_loader(kernel.arch().architecture(), elf); !status.ok())
    {
        return status;
    }
    if (auto status = verify_wait_queue(); !status.ok())
    {
        return status;
    }
    if (auto status = verify_process_lifecycle(kernel, elf, processes); !status.ok())
    {
        return status;
    }
    if (auto status = verify_userland_smoke(kernel, elf, processes); !status.ok())
    {
        return status;
    }
    if (auto status = verify_user_management_and_isolation(kernel, elf, processes); !status.ok())
    {
        return status;
    }
    if (auto status = verify_background_programs_and_posix(kernel); !status.ok())
    {
        return status;
    }

    report.proc = true;
    report.elf = true;
    report.userland = true;
    return Status::success();
}

} // namespace ok
