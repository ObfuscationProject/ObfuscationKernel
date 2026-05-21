#include "ok/sched/process.hpp"

#include <cstddef>

namespace ok::sched
{
namespace
{

u8 byte_at(std::span<const std::byte> image, usize offset)
{
    return offset < image.size() ? std::to_integer<u8>(image[offset]) : 0;
}

u16 read_le16(std::span<const std::byte> image, usize offset)
{
    return static_cast<u16>(byte_at(image, offset) | static_cast<u16>(byte_at(image, offset + 1)) << 8);
}

u64 read_le64(std::span<const std::byte> image, usize offset)
{
    u64 value = 0;
    for (usize i = 0; i < 8; ++i)
    {
        value |= static_cast<u64>(byte_at(image, offset + i)) << (i * 8);
    }
    return value;
}

u16 elf_machine_for(arch::Architecture architecture)
{
    switch (architecture)
    {
    case arch::Architecture::x86_64:
        return 0x3e;
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
    }
    return 0;
}

} // namespace

Status FileDescriptorTable::open_slot(FdLike fd)
{
    if (fd < 0 || static_cast<usize>(fd) >= open_.size())
    {
        return Status::invalid_argument("file descriptor is out of range");
    }
    auto &slot = open_[static_cast<usize>(fd)];
    if (!slot)
    {
        slot = true;
        ++open_count_;
    }
    return Status::success();
}

Status FileDescriptorTable::clone_from(const FileDescriptorTable &source)
{
    open_ = source.open_;
    open_count_ = source.open_count_;
    return Status::success();
}

bool FileDescriptorTable::contains(FdLike fd) const
{
    return fd >= 0 && static_cast<usize>(fd) < open_.size() && open_[static_cast<usize>(fd)];
}

Status MemoryMap::add_area(memory::VmArea area)
{
    if (area.length == 0)
    {
        return Status::invalid_argument("memory map area must be non-empty");
    }
    return areas_.push_back(area);
}

Status MemoryMap::clone_from(const MemoryMap &source)
{
    areas_ = {};
    for (const auto &area : source.areas_)
    {
        if (auto status = areas_.push_back(area); !status.ok())
        {
            return status;
        }
    }
    return Status::success();
}

Status MemoryMap::replace_with(memory::VmArea area)
{
    areas_ = {};
    return add_area(area);
}

const memory::VmArea *MemoryMap::area(usize index) const
{
    return index < areas_.size() ? &areas_[index] : nullptr;
}

Status WaitQueue::block(ProcessId pid)
{
    if (blocked(pid))
    {
        return Status::already_exists("process is already blocked on wait queue");
    }
    return blocked_.push_back(pid);
}

Status WaitQueue::wake(ProcessId pid)
{
    for (usize i = 0; i < blocked_.size(); ++i)
    {
        if (blocked_[i] == pid)
        {
            return blocked_.erase_at(i);
        }
    }
    return Status::not_found("process is not blocked on wait queue");
}

bool WaitQueue::blocked(ProcessId pid) const
{
    for (const auto blocked_pid : blocked_)
    {
        if (blocked_pid == pid)
        {
            return true;
        }
    }
    return false;
}

Status Process::configure(ProcessId pid, ProcessId parent, std::string_view name)
{
    pid_ = pid;
    parent_pid_ = parent;
    group_ = ProcessGroup{.id = parent == 0 ? pid : parent};
    session_ = Session{.id = group_.id};
    credentials_ = {};
    signals_ = {};
    fd_table_ = {};
    memory_map_ = {};
    threads_ = {};
    children_ = {};
    state_ = TaskState::created;
    exit_code_ = 0;
    return name_.assign(name);
}

Status Process::add_thread(ThreadId tid, arch::CpuContext context, TaskState state)
{
    return threads_.push_back(Thread{.tid = tid, .process = pid_, .state = state, .context = context});
}

Status Process::add_child(ProcessId child)
{
    for (const auto pid : children_)
    {
        if (pid == child)
        {
            return Status::already_exists("child process already linked");
        }
    }
    return children_.push_back(child);
}

Status Process::remove_child(ProcessId child)
{
    for (usize i = 0; i < children_.size(); ++i)
    {
        if (children_[i] == child)
        {
            return children_.erase_at(i);
        }
    }
    return Status::not_found("child process not linked");
}

Result<LoadedElf> ElfLoader::load(std::span<const std::byte> image, arch::Architecture architecture) const
{
    if (image.size() < 64)
    {
        return Status::invalid_argument("ELF image is too small");
    }
    if (byte_at(image, 0) != 0x7f || byte_at(image, 1) != 'E' || byte_at(image, 2) != 'L' ||
        byte_at(image, 3) != 'F')
    {
        return Status::invalid_argument("ELF magic is invalid");
    }
    if (byte_at(image, 4) != 2)
    {
        return Status::unsupported("ELF32 loading is not implemented in this profile");
    }
    if (read_le16(image, 18) != elf_machine_for(architecture))
    {
        return Status::invalid_argument("ELF architecture mismatch");
    }
    const auto entry = read_le64(image, 24);
    const auto phentsize = read_le16(image, 54);
    const auto phnum = read_le16(image, 56);
    if (phnum == 0 || phentsize == 0)
    {
        return Status::invalid_argument("ELF program headers are missing");
    }
    const auto entry_address = entry == 0 ? static_cast<uptr>(0x400000) : static_cast<uptr>(entry);
    return LoadedElf{.entry = entry_address, .stack_pointer = static_cast<uptr>(0x800000), .load_segments = phnum};
}

Result<ProcessId> ProcessManager::allocate_process(std::string_view name, ProcessId parent)
{
    if (processes_.full())
    {
        return Status::overflow("process table capacity exceeded");
    }
    Process process;
    const auto pid = next_pid_++;
    if (auto status = process.configure(pid, parent, name); !status.ok())
    {
        return status;
    }
    if (auto status = process.fd_table().open_slot(0); !status.ok())
    {
        return status;
    }
    static_cast<void>(process.fd_table().open_slot(1));
    static_cast<void>(process.fd_table().open_slot(2));
    if (auto status = processes_.push_back(process); !status.ok())
    {
        return status;
    }
    if (auto *parent_process = find(parent); parent_process != nullptr)
    {
        if (auto status = parent_process->add_child(pid); !status.ok())
        {
            return status;
        }
    }
    return pid;
}

Result<ProcessId> ProcessManager::create_kernel_thread(std::string_view name, arch::CpuContext context)
{
    auto pid = allocate_process(name, 0);
    if (!pid)
    {
        return pid.status();
    }
    auto *process = find(pid.value());
    if (process == nullptr)
    {
        return Status::fault("created process is not visible");
    }
    if (auto status = process->add_thread(next_tid_++, context, TaskState::runnable); !status.ok())
    {
        return status;
    }
    process->set_state(TaskState::runnable);
    return pid.value();
}

Result<ProcessId> ProcessManager::create_user_process(std::string_view name, std::span<const std::byte> elf_image,
                                                      arch::Architecture architecture)
{
    auto loaded = elf_loader_.load(elf_image, architecture);
    if (!loaded)
    {
        return loaded.status();
    }
    auto &ops = arch::arch_operations(architecture);
    auto context = ops.make_user_context(arch::UserEntry{
        .instruction_pointer = loaded.value().entry,
        .stack_pointer = loaded.value().stack_pointer,
        .argument = 0,
    });
    auto pid = create_kernel_thread(name, context);
    if (!pid)
    {
        return pid.status();
    }
    auto *process = find(pid.value());
    if (process == nullptr)
    {
        return Status::fault("created user process is not visible");
    }
    process->threads()[0].state = TaskState::runnable;
    if (auto status = process->memory_map().replace_with(memory::VmArea{
            .base = 0x400000,
            .length = 0x2000,
            .permissions = memory::page_read | memory::page_execute | memory::page_user,
        });
        !status.ok())
    {
        return status;
    }
    return pid.value();
}

Result<ThreadId> ProcessManager::create_user_thread(ProcessId pid, arch::CpuContext context)
{
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    const auto tid = next_tid_++;
    if (auto status = process->add_thread(tid, context, TaskState::runnable); !status.ok())
    {
        return status;
    }
    process->set_state(TaskState::runnable);
    return tid;
}

Result<ProcessId> ProcessManager::fork(ProcessId parent)
{
    auto *parent_process = find(parent);
    if (parent_process == nullptr)
    {
        return Status::not_found("parent process not found");
    }
    auto child = allocate_process(parent_process->name(), parent);
    if (!child)
    {
        return child.status();
    }
    auto *child_process = find(child.value());
    if (child_process == nullptr)
    {
        return Status::fault("forked process is not visible");
    }
    if (auto status = child_process->fd_table().clone_from(parent_process->fd_table()); !status.ok())
    {
        return status;
    }
    if (auto status = child_process->memory_map().clone_from(parent_process->memory_map()); !status.ok())
    {
        return status;
    }
    if (!parent_process->threads().empty())
    {
        if (auto status =
                child_process->add_thread(next_tid_++, parent_process->threads()[0].context, TaskState::runnable);
            !status.ok())
        {
            return status;
        }
    }
    child_process->set_state(TaskState::runnable);
    return child.value();
}

Status ProcessManager::execve(ProcessId pid, std::span<const std::byte> elf_image, arch::Architecture architecture)
{
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    auto loaded = elf_loader_.load(elf_image, architecture);
    if (!loaded)
    {
        return loaded.status();
    }
    if (auto status = process->memory_map().replace_with(memory::VmArea{
            .base = 0x400000,
            .length = 0x2000,
            .permissions = memory::page_read | memory::page_execute | memory::page_user,
        });
        !status.ok())
    {
        return status;
    }
    auto &ops = arch::arch_operations(architecture);
    if (process->threads().empty())
    {
        return process->add_thread(next_tid_++, ops.make_user_context(arch::UserEntry{
                                                    .instruction_pointer = loaded.value().entry,
                                                    .stack_pointer = loaded.value().stack_pointer,
                                                    .argument = 0,
                                                }),
                                   TaskState::runnable);
    }
    process->threads()[0].context = ops.make_user_context(arch::UserEntry{
        .instruction_pointer = loaded.value().entry,
        .stack_pointer = loaded.value().stack_pointer,
        .argument = 0,
    });
    process->threads()[0].state = TaskState::runnable;
    process->set_state(TaskState::runnable);
    return Status::success();
}

Status ProcessManager::exit(ProcessId pid, i32 code)
{
    auto *process = find(pid);
    if (process == nullptr)
    {
        return Status::not_found("process not found");
    }
    process->set_exit_code(code);
    process->set_state(TaskState::zombie);
    for (auto &thread : process->threads())
    {
        thread.state = TaskState::exited;
    }
    return Status::success();
}

Status ProcessManager::exit_group(ProcessId pid, i32 code)
{
    return exit(pid, code);
}

Result<i32> ProcessManager::wait4(ProcessId parent, ProcessId child)
{
    auto *parent_process = find(parent);
    auto *child_process = find(child);
    if (parent_process == nullptr || child_process == nullptr || child_process->parent_pid() != parent)
    {
        return Status::not_found("wait child not found");
    }
    if (child_process->state() != TaskState::zombie && child_process->state() != TaskState::exited)
    {
        return Status::would_block("child has not exited");
    }
    const auto code = child_process->exit_code();
    if (auto status = parent_process->remove_child(child); !status.ok())
    {
        return status;
    }
    if (auto status = cleanup_zombie(child); !status.ok())
    {
        return status;
    }
    return code;
}

Status ProcessManager::reparent_children(ProcessId from, ProcessId to)
{
    auto *target = find(to);
    if (target == nullptr)
    {
        return Status::not_found("reparent target not found");
    }
    for (auto &process : processes_)
    {
        if (process.parent_pid() == from)
        {
            process.set_parent(to);
            if (auto status = target->add_child(process.pid()); !status.ok() && status.code() != StatusCode::already_exists)
            {
                return status;
            }
        }
    }
    return Status::success();
}

Status ProcessManager::cleanup_zombie(ProcessId pid)
{
    for (usize i = 0; i < processes_.size(); ++i)
    {
        if (processes_[i].pid() == pid)
        {
            if (processes_[i].state() != TaskState::zombie && processes_[i].state() != TaskState::exited)
            {
                return Status::invalid_argument("process is not a zombie");
            }
            return processes_.erase_at(i);
        }
    }
    return Status::not_found("process not found");
}

Process *ProcessManager::find(ProcessId pid)
{
    for (auto &process : processes_)
    {
        if (process.pid() == pid)
        {
            return &process;
        }
    }
    return nullptr;
}

const Process *ProcessManager::find(ProcessId pid) const
{
    for (const auto &process : processes_)
    {
        if (process.pid() == pid)
        {
            return &process;
        }
    }
    return nullptr;
}

usize ProcessManager::zombie_count() const
{
    usize count = 0;
    for (const auto &process : processes_)
    {
        if (process.state() == TaskState::zombie)
        {
            ++count;
        }
    }
    return count;
}

std::string_view task_state_name(TaskState state)
{
    switch (state)
    {
    case TaskState::created:
        return "created";
    case TaskState::runnable:
        return "runnable";
    case TaskState::running:
        return "running";
    case TaskState::blocked:
        return "blocked";
    case TaskState::zombie:
        return "zombie";
    case TaskState::exited:
        return "exited";
    }
    return "unknown";
}

} // namespace ok::sched
