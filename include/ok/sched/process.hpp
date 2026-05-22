#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/memory/memory.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/user/user.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok::sched
{

inline constexpr usize max_process_threads = 8;
inline constexpr usize max_process_children = 16;
inline constexpr usize max_memory_areas = 16;
inline constexpr usize max_process_fds = 32;

enum class TaskState : u8
{
    created,
    runnable,
    running,
    blocked,
    zombie,
    exited,
};

using Credentials = user::Credentials;

struct SignalState
{
    u64 pending{0};
    u64 blocked{0};
};

struct ProcessGroup
{
    u64 id{0};
};

struct Session
{
    u64 id{0};
};

class FileDescriptorTable final
{
  public:
    using FdLike = i32;

    Status open_slot(FdLike fd);
    Status clone_from(const FileDescriptorTable &source);
    [[nodiscard]] bool contains(FdLike fd) const;
    [[nodiscard]] usize open_count() const
    {
        return open_count_;
    }

  private:
    std::array<bool, max_process_fds> open_{};
    usize open_count_{0};
};

class MemoryMap final
{
  public:
    Status add_area(memory::VmArea area);
    Status clone_from(const MemoryMap &source);
    Status replace_with(memory::VmArea area);
    [[nodiscard]] bool contains(uptr address, usize length) const;
    [[nodiscard]] bool allows_user_access(uptr address, usize length, usize permissions) const;
    [[nodiscard]] Status require_user_access(uptr address, usize length, usize permissions) const;
    [[nodiscard]] usize area_count() const
    {
        return areas_.size();
    }
    [[nodiscard]] const memory::VmArea *area(usize index) const;

  private:
    StaticVector<memory::VmArea, max_memory_areas> areas_;
};

class WaitQueue final
{
  public:
    Status block(ProcessId pid);
    Status wake(ProcessId pid);
    [[nodiscard]] bool blocked(ProcessId pid) const;

  private:
    StaticVector<ProcessId, max_process_children> blocked_;
};

struct Task
{
    ThreadId tid{0};
    ProcessId process{0};
    TaskState state{TaskState::created};
    arch::CpuContext context{};
};

using Thread = Task;

class Process final
{
  public:
    Status configure(ProcessId pid, ProcessId parent, std::string_view name);
    Status add_thread(ThreadId tid, arch::CpuContext context, TaskState state);
    Status add_child(ProcessId child);
    Status remove_child(ProcessId child);
    [[nodiscard]] ProcessId pid() const
    {
        return pid_;
    }
    [[nodiscard]] ProcessId parent_pid() const
    {
        return parent_pid_;
    }
    void set_parent(ProcessId parent)
    {
        parent_pid_ = parent;
    }
    [[nodiscard]] std::string_view name() const
    {
        return name_.view();
    }
    [[nodiscard]] TaskState state() const
    {
        return state_;
    }
    void set_state(TaskState state)
    {
        state_ = state;
    }
    void set_exit_code(i32 code)
    {
        exit_code_ = code;
    }
    [[nodiscard]] i32 exit_code() const
    {
        return exit_code_;
    }
    [[nodiscard]] StaticVector<Thread, max_process_threads> &threads()
    {
        return threads_;
    }
    [[nodiscard]] const StaticVector<Thread, max_process_threads> &threads() const
    {
        return threads_;
    }
    [[nodiscard]] StaticVector<ProcessId, max_process_children> &children()
    {
        return children_;
    }
    [[nodiscard]] FileDescriptorTable &fd_table()
    {
        return fd_table_;
    }
    [[nodiscard]] const FileDescriptorTable &fd_table() const
    {
        return fd_table_;
    }
    [[nodiscard]] MemoryMap &memory_map()
    {
        return memory_map_;
    }
    [[nodiscard]] const MemoryMap &memory_map() const
    {
        return memory_map_;
    }
    [[nodiscard]] Credentials &credentials()
    {
        return credentials_;
    }
    [[nodiscard]] const Credentials &credentials() const
    {
        return credentials_;
    }
    void set_credentials(Credentials credentials)
    {
        credentials_ = credentials;
    }
    [[nodiscard]] SignalState &signals()
    {
        return signals_;
    }

  private:
    ProcessId pid_{0};
    ProcessId parent_pid_{0};
    ProcessGroup group_{};
    Session session_{};
    Credentials credentials_{};
    SignalState signals_{};
    FileDescriptorTable fd_table_{};
    MemoryMap memory_map_{};
    StaticVector<Thread, max_process_threads> threads_;
    StaticVector<ProcessId, max_process_children> children_;
    FixedString<max_process_name> name_{};
    TaskState state_{TaskState::created};
    i32 exit_code_{0};
};

struct LoadedElf
{
    uptr entry{0};
    uptr stack_pointer{0};
    usize load_segments{0};
};

class ElfLoader final
{
  public:
    Result<LoadedElf> load(std::span<const std::byte> image, arch::Architecture architecture) const;
};

class ProcessManager final
{
  public:
    Result<ProcessId> create_kernel_thread(std::string_view name, arch::CpuContext context);
    Result<ProcessId> create_user_process(std::string_view name, std::span<const std::byte> elf_image,
                                          arch::Architecture architecture);
    Result<ThreadId> create_user_thread(ProcessId pid, arch::CpuContext context);
    Result<ProcessId> fork(ProcessId parent);
    Status execve(ProcessId pid, std::span<const std::byte> elf_image, arch::Architecture architecture);
    Status set_credentials(ProcessId pid, user::Credentials credentials);
    Result<i32> wait4(ProcessId parent, ProcessId child);
    Status exit(ProcessId pid, i32 code);
    Status exit_group(ProcessId pid, i32 code);
    Status reparent_children(ProcessId from, ProcessId to);
    Status cleanup_zombie(ProcessId pid);

    [[nodiscard]] Process *find(ProcessId pid);
    [[nodiscard]] const Process *find(ProcessId pid) const;
    [[nodiscard]] usize process_count() const
    {
        return processes_.size();
    }
    [[nodiscard]] usize zombie_count() const;

  private:
    Result<ProcessId> allocate_process(std::string_view name, ProcessId parent);

    StaticVector<Process, max_processes> processes_;
    ProcessId next_pid_{1};
    ThreadId next_tid_{1};
    ElfLoader elf_loader_;
};

[[nodiscard]] std::string_view task_state_name(TaskState state);

} // namespace ok::sched
