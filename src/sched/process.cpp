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

u64 read_integer(std::span<const std::byte> image, usize offset, usize width, bool big_endian)
{
    u64 value = 0;
    for (usize i = 0; i < width; ++i)
    {
        const auto shift = (big_endian ? width - 1 - i : i) * 8;
        value |= static_cast<u64>(byte_at(image, offset + i)) << shift;
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

bool range_end(uptr base, usize length, uptr &end)
{
    if (length == 0)
    {
        return false;
    }
    end = base + static_cast<uptr>(length);
    return end >= base;
}

bool ranges_overlap(memory::VmArea lhs, memory::VmArea rhs)
{
    uptr lhs_end = 0;
    uptr rhs_end = 0;
    if (!range_end(lhs.base, lhs.length, lhs_end) || !range_end(rhs.base, rhs.length, rhs_end))
    {
        return true;
    }
    return lhs.base < rhs_end && rhs.base < lhs_end;
}

usize elf_segment_permissions(u32 flags)
{
    usize permissions = memory::page_user;
    if ((flags & 0x4u) != 0)
    {
        permissions |= memory::page_read;
    }
    if ((flags & 0x2u) != 0)
    {
        permissions |= memory::page_write;
    }
    if ((flags & 0x1u) != 0)
    {
        permissions |= memory::page_execute;
    }
    return permissions;
}

} // namespace

Status FileDescriptorTable::open_slot(FdLike fd)
{
    auto *slot = entry(fd);
    if (slot == nullptr)
        return Status::invalid_argument("file descriptor is out of range");
    slot->used = true;
    return Status::success();
}
Status FileDescriptorTable::close_slot(FdLike fd)
{
    auto *slot = entry(fd);
    if (slot == nullptr || !slot->used)
        return Status::not_found("file descriptor is not open");
    if (slot->description != invalid_description)
        return Status::invalid_argument("managed descriptors must be closed by the scheduler");
    *slot = {};
    return Status::success();
}
Status FileDescriptorTable::clone_from(const FileDescriptorTable &source)
{
    if (this == &source)
        return Status::success();
    for (usize i = 0; i < entries_.size(); ++i)
        if ((entries_[i].used && entries_[i].description != invalid_description) ||
            (source.entries_[i].used && source.entries_[i].description != invalid_description))
            return Status::invalid_argument("managed descriptors must be cloned by the scheduler");
    entries_ = source.entries_;
    return Status::success();
}
bool FileDescriptorTable::contains(FdLike fd) const
{
    const auto *slot = entry(fd);
    return slot != nullptr && slot->used;
}
usize FileDescriptorTable::open_count() const
{
    usize count = 0;
    for (const auto &slot : entries_)
        if (slot.used)
            ++count;
    return count;
}

Status MemoryMap::add_area(memory::VmArea area)
{
    if (area.length == 0)
    {
        return Status::invalid_argument("memory map area must be non-empty");
    }
    uptr ignored_end = 0;
    if (!range_end(area.base, area.length, ignored_end))
    {
        return Status::invalid_argument("memory map area wraps address space");
    }
    for (const auto &existing : areas_)
    {
        if (ranges_overlap(existing, area))
        {
            return Status::already_exists("memory map area overlaps an existing area");
        }
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
    MemoryMap replacement;
    if (auto status = replacement.add_area(area); !status.ok())
        return status;
    areas_ = replacement.areas_;
    return Status::success();
}

const memory::VmArea *MemoryMap::area(usize index) const
{
    return index < areas_.size() ? &areas_[index] : nullptr;
}

bool MemoryMap::contains(uptr address, usize length) const
{
    for (const auto &area : areas_)
    {
        if (area.contains(address, length))
        {
            return true;
        }
    }
    return false;
}

bool MemoryMap::allows_user_access(uptr address, usize length, usize permissions) const
{
    for (const auto &area : areas_)
    {
        if (area.contains(address, length) && (area.permissions & memory::page_user) != 0 &&
            (area.permissions & permissions) == permissions)
        {
            return true;
        }
    }
    return false;
}

Status MemoryMap::require_user_access(uptr address, usize length, usize permissions) const
{
    if (!contains(address, length))
    {
        return Status::not_found("user address is not mapped in this process");
    }
    return allows_user_access(address, length, permissions) ? Status::success()
                                                            : Status::denied("process memory access denied");
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

Status ProcessControlBlock::configure(ProcessId pid, ProcessId parent, std::string_view name)
{
    pid_ = pid;
    parent_pid_ = parent;
    group_ = ProcessGroup{.id = parent == 0 ? pid : parent};
    session_ = Session{.id = group_.id};
    credentials_ = user::kernel_credentials();
    signals_ = {};
    resources_ = {};
    threads_ = {};
    children_ = {};
    state_ = TaskState::created;
    exit_code_ = 0;
    termination_signal_ = 0;
    return name_.assign(name);
}

Status ProcessControlBlock::add_thread(ThreadId tid, arch::CpuContext context, TaskState state)
{
    return threads_.push_back(ThreadControlBlock{.tid = tid, .owner = pid_, .state = state, .context = context});
}

Status ProcessControlBlock::add_child(ProcessId child)
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

Status ProcessControlBlock::remove_child(ProcessId child)
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
        return Status::invalid_argument("ELF image is too small");
    if (byte_at(image, 0) != 0x7f || byte_at(image, 1) != 'E' || byte_at(image, 2) != 'L' || byte_at(image, 3) != 'F')
        return Status::invalid_argument("ELF magic is invalid");
    if (byte_at(image, 4) != 2)
        return Status::unsupported("ELF32 loading is not implemented in this profile");
    const auto encoding = byte_at(image, 5);
    if (encoding != 1 && encoding != 2)
        return Status::invalid_argument("ELF byte order is invalid");
    const bool big_endian = encoding == 2;
    auto read16 = [&](usize offset) { return read_integer(image, offset, 2, big_endian); };
    auto read32 = [&](usize offset) { return read_integer(image, offset, 4, big_endian); };
    auto read64 = [&](usize offset) { return read_integer(image, offset, 8, big_endian); };
    if (byte_at(image, 6) != 1 || read32(20) != 1 || read16(52) != 64)
        return Status::invalid_argument("ELF version or header size is invalid");
    if (read16(18) != elf_machine_for(architecture))
        return Status::invalid_argument("ELF architecture mismatch");
    const auto type = read16(16);
    if (type != 2 && type != 3)
        return Status::unsupported("ELF image is not executable");
    const auto entry = read64(24);
    const auto phoff64 = read64(32);
    const auto phentsize = read16(54);
    const auto phnum = read16(56);
    constexpr u64 max_address = static_cast<u64>(~uptr{0});
    if (entry == 0 || entry > max_address)
        return Status::invalid_argument("ELF entry is invalid");
    if (phnum == 0 || phentsize < 56 || phoff64 < 64 || phoff64 > image.size() ||
        phnum > (image.size() - static_cast<usize>(phoff64)) / phentsize)
        return Status::invalid_argument("ELF program header table is out of range");
    LoadedElf loaded{.entry = static_cast<uptr>(entry),
                     .stack_pointer = static_cast<uptr>(0x800000),
                     .static_executable = type == 2};
    bool entry_executable = false;
    for (usize i = 0; i < phnum; ++i)
    {
        const auto offset = static_cast<usize>(phoff64) + i * static_cast<usize>(phentsize);
        if (read32(offset) != 1)
            continue;
        const auto flags = static_cast<u32>(read32(offset + 4));
        const auto file_offset = read64(offset + 8);
        const auto virtual_address = read64(offset + 16);
        const auto file_size = read64(offset + 32);
        const auto memory_size = read64(offset + 40);
        const auto alignment = read64(offset + 48);
        if (memory_size < file_size || file_offset > image.size() || file_size > image.size() - file_offset ||
            virtual_address > max_address || memory_size > max_address - virtual_address)
            return Status::invalid_argument("ELF load segment is out of range");
        if (alignment > 1 && ((alignment & (alignment - 1)) != 0 ||
                              (virtual_address & (alignment - 1)) != (file_offset & (alignment - 1))))
            return Status::invalid_argument("ELF segment alignment is invalid");
        if (memory_size == 0)
            continue;
        const memory::VmArea area{.base = static_cast<uptr>(virtual_address),
                                  .length = static_cast<usize>(memory_size),
                                  .permissions = elf_segment_permissions(flags)};
        for (const auto &segment : loaded.segments)
            if (ranges_overlap(area, memory::VmArea{.base = segment.virtual_address, .length = segment.memory_size}))
                return Status::invalid_argument("ELF load segments overlap");
        if (auto status = loaded.segments.push_back(LoadedElf::Segment{.virtual_address = area.base,
                                                                       .memory_size = area.length,
                                                                       .file_size = static_cast<usize>(file_size),
                                                                       .file_offset = static_cast<usize>(file_offset),
                                                                       .permissions = area.permissions});
            !status.ok())
            return status;
        if ((flags & 1u) && entry >= virtual_address && entry - virtual_address < memory_size)
            entry_executable = true;
    }
    if (loaded.segments.empty())
        return Status::invalid_argument("ELF image has no loadable segments");
    if (!entry_executable)
        return Status::invalid_argument("ELF entry is not in an executable segment");
    loaded.load_segments = loaded.segments.size();
    return loaded;
}

Result<ProcessId> ProcessManager::create_kernel_thread(std::string_view name, arch::CpuContext context)
{
    auto pid = scheduler_->create_process(name, context);
    if (!pid)
        return pid.status();
    if (auto status = scheduler_->set_runnable(pid.value()); !status.ok())
    {
        static_cast<void>(scheduler_->kill_process(pid.value()));
        return status;
    }
    return pid;
}

namespace
{
Result<MemoryMap> elf_memory_map(const LoadedElf &loaded)
{
    MemoryMap map;
    for (const auto &segment : loaded.segments)
    {
        if (auto status = map.add_area(memory::VmArea{
                .base = segment.virtual_address, .length = segment.memory_size, .permissions = segment.permissions});
            !status.ok())
            return status;
    }
    return map;
}
} // namespace

Result<ProcessId> ProcessManager::create_user_process(std::string_view name, std::span<const std::byte> elf_image,
                                                      arch::Architecture architecture)
{
    if (auto status = arch::validate_architecture(architecture); !status.ok())
        return status;
    auto loaded = elf_loader_.load(elf_image, architecture);
    if (!loaded)
        return loaded.status();
    auto map = elf_memory_map(loaded.value());
    if (!map)
        return map.status();
    const auto context = arch::configured_arch_operations().make_user_context(
        arch::UserEntry{.instruction_pointer = loaded.value().entry, .stack_pointer = loaded.value().stack_pointer});
    auto pid = scheduler_->create_user_process(name, context);
    if (!pid)
        return pid.status();
    auto *process = find(pid.value());
    process->memory_map() = map.value();
    process->set_credentials(user::default_user_credentials());
    static_cast<void>(scheduler_->set_runnable(pid.value()));
    return pid;
}

Result<ThreadId> ProcessManager::create_user_thread(ProcessId pid, arch::CpuContext context)
{
    if (context.mode != arch::PrivilegeMode::user)
        return Status::invalid_argument("user thread context must use user privilege");
    return scheduler_->create_thread(pid, context);
}
Result<ProcessId> ProcessManager::fork(ProcessId parent)
{
    return scheduler_->fork_process(parent);
}

Status ProcessManager::execve(ProcessId pid, std::span<const std::byte> elf_image, arch::Architecture architecture)
{
    if (auto status = arch::validate_architecture(architecture); !status.ok())
        return status;
    if (find(pid) == nullptr)
        return Status::not_found("process not found");
    auto loaded = elf_loader_.load(elf_image, architecture);
    if (!loaded)
        return loaded.status();
    auto map = elf_memory_map(loaded.value());
    if (!map)
        return map.status();
    const auto context = arch::configured_arch_operations().make_user_context(
        arch::UserEntry{.instruction_pointer = loaded.value().entry, .stack_pointer = loaded.value().stack_pointer});
    return scheduler_->replace_image(pid, context, map.value());
}
Status ProcessManager::set_credentials(ProcessId pid, user::Credentials credentials)
{
    return scheduler_->set_credentials(pid, credentials);
}
Status ProcessManager::exit(ProcessId pid, i32 code)
{
    return scheduler_->exit_process(pid, code);
}
Status ProcessManager::exit_group(ProcessId pid, i32 code)
{
    return exit(pid, code);
}
Result<i32> ProcessManager::wait4(ProcessId parent, ProcessId child)
{
    return scheduler_->wait_process(parent, child);
}
Status ProcessManager::reparent_children(ProcessId from, ProcessId to)
{
    return scheduler_->reparent_children(from, to);
}
Status ProcessManager::cleanup_zombie(ProcessId pid)
{
    return scheduler_->reap_process(pid);
}
Process *ProcessManager::find(ProcessId pid)
{
    return scheduler_->find(pid);
}
const Process *ProcessManager::find(ProcessId pid) const
{
    return scheduler_->find(pid);
}
usize ProcessManager::zombie_count() const
{
    usize count = 0;
    for (const auto &process : scheduler_->processes())
        if (process.state() == TaskState::zombie)
            ++count;
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
