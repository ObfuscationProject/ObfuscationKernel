#include "kernel_roadmap_tests.hpp"

#include "ok/posix/posix.hpp"
#include "ok/sched/process.hpp"
#include "ok/syscall/syscall.hpp"

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
        execed_child->parent_pid() != user.value() || execed_child->threads()[0].context.mode != arch::PrivilegeMode::user)
    {
        return Status::fault("execve did not replace address space while preserving selected metadata");
    }

    auto extra_thread =
        processes.create_user_thread(child.value(), ops.make_user_context(arch::UserEntry{
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

    static_cast<void>(kernel.posix().unlink("/tmp/p3_fd.txt"));
    auto fd = kernel.posix().open("/tmp/p3_fd.txt", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC, 0644);
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

    report.proc = true;
    report.elf = true;
    report.userland = true;
    return Status::success();
}

} // namespace ok
