#pragma once
#include "ok/sched/scheduler.hpp"
namespace ok::sched
{
inline constexpr usize max_process_threads = max_threads_per_process;
using Credentials = user::Credentials;
using TaskState = ProcessState;
using Task = ThreadControlBlock;
using Thread = ThreadControlBlock;
using Process = ProcessControlBlock;
class WaitQueue final
{
  public:
    Status block(ProcessId pid);
    Status wake(ProcessId pid);
    [[nodiscard]] bool blocked(ProcessId pid) const;

  private:
    StaticVector<ProcessId, max_process_children> blocked_;
};

struct LoadedElf
{
    struct Segment
    {
        uptr virtual_address{0};
        usize memory_size{0};
        usize file_size{0};
        usize file_offset{0};
        usize permissions{0};
    };

    uptr entry{0};
    uptr stack_pointer{0};
    usize load_segments{0};
    StaticVector<Segment, max_elf_load_segments> segments{};
    bool static_executable{true};
};

class ElfLoader final
{
  public:
    Result<LoadedElf> load(std::span<const std::byte> image, arch::Architecture architecture) const;
    // The prefix must contain the ELF header and complete program-header table.
    // Segment extents are validated against the independent complete file size.
    Result<LoadedElf> load_prefix(std::span<const std::byte> prefix, usize file_bytes,
                                  arch::Architecture architecture) const;
};

class ProcessManager final
{
  public:
    explicit ProcessManager(Scheduler &scheduler) : scheduler_(&scheduler)
    {
    }
    ProcessManager(const ProcessManager &) = delete;
    ProcessManager &operator=(const ProcessManager &) = delete;
    [[nodiscard]] Scheduler &scheduler()
    {
        return *scheduler_;
    }
    Result<ProcessId> create_kernel_thread(std::string_view name, arch::CpuContext context);
    Result<ProcessId> create_user_process(std::string_view name, std::span<const std::byte> elf_image,
                                          arch::Architecture architecture);
    Result<ThreadId> create_user_thread(ProcessId pid, arch::CpuContext context);
    Result<ProcessId> fork(ProcessId parent, bool publish = true);
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
        return scheduler_->process_count();
    }
    [[nodiscard]] usize zombie_count() const;

  private:
    Scheduler *scheduler_;
    ElfLoader elf_loader_;
};

[[nodiscard]] std::string_view task_state_name(TaskState state);

} // namespace ok::sched
