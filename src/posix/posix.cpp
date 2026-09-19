#include "ok/posix/posix.hpp"
namespace ok::posix
{
Status PosixService::initialize(fs::VirtualFileSystem &vfs, driver::ConsoleDriver &console, sched::Scheduler &scheduler)
{
    vfs_ = &vfs;
    console_ = &console;
    scheduler_ = &scheduler;
    initialized_ = true;
    monotonic_ticks_ = 0;
    boot_tls_ = {};
    boot_credentials_ = user::kernel_credentials();
    return Status::success();
}
sched::ProcessId PosixService::getpid() const
{
    return scheduler_ == nullptr ? 0 : scheduler_->current_pid();
}
sched::ProcessId PosixService::getppid() const
{
    const auto *process = scheduler_ == nullptr ? nullptr : scheduler_->find(getpid());
    return process == nullptr ? 0 : process->parent_pid();
}
sched::ProcessResources &PosixService::resources()
{
    if (scheduler_ == nullptr)
        return unbound_resources_;
    auto *process = scheduler_->find(getpid());
    return process == nullptr ? scheduler_->boot_resources() : process->resources();
}
const sched::ProcessResources &PosixService::resources() const
{
    if (scheduler_ == nullptr)
        return unbound_resources_;
    const auto *process = scheduler_->find(getpid());
    return process == nullptr ? scheduler_->boot_resources() : process->resources();
}
sched::ThreadLocalState &PosixService::thread_local_state()
{
    auto *process = scheduler_ == nullptr ? nullptr : scheduler_->find(getpid());
    if (process == nullptr || process->threads().empty())
        return boot_tls_;
    for (auto &thread : process->threads())
        if (thread.tid == scheduler_->current_tid(0))
            return thread.tls;
    return process->threads()[0].tls;
}
user::Credentials PosixService::user_credentials() const
{
    const auto *process = scheduler_ == nullptr ? nullptr : scheduler_->find(getpid());
    return process == nullptr ? boot_credentials_ : process->credentials();
}
Status PosixService::set_identity(u32 uid, u32 gid)
{
    return set_credentials(user::Credentials{.uid = uid, .gid = gid, .euid = uid, .egid = gid, .kernel_space = false});
}
Status PosixService::set_credentials(user::Credentials credentials)
{
    auto *process = scheduler_ == nullptr ? nullptr : scheduler_->find(getpid());
    if (process == nullptr)
        boot_credentials_ = credentials;
    else
        process->set_credentials(credentials);
    return Status::success();
}
Status PosixService::activate(sched::ProcessId pid)
{
    return scheduler_ == nullptr ? Status::not_initialized("POSIX service not initialized") : scheduler_->activate(pid);
}
Result<sched::ProcessId> PosixService::fork()
{
    return fork(getpid());
}
Result<sched::ProcessId> PosixService::fork(sched::ProcessId parent)
{
    if (scheduler_ == nullptr)
        return Status::not_initialized("POSIX service not initialized");
    return scheduler_->fork_process(parent);
}
Status PosixService::exec(sched::ProcessId pid, arch::CpuContext context, const sched::MemoryMap &memory)
{
    return scheduler_ == nullptr ? Status::not_initialized("POSIX service not initialized")
                                 : scheduler_->replace_image(pid, context, memory);
}
Status PosixService::exit(sched::ProcessId pid, i32 code)
{
    return scheduler_ == nullptr ? Status::not_initialized("POSIX service not initialized")
                                 : scheduler_->exit_process(pid, code);
}
Result<i32> PosixService::wait(sched::ProcessId parent, sched::ProcessId child)
{
    if (scheduler_ == nullptr)
        return Status::not_initialized("POSIX service not initialized");
    return scheduler_->wait_process(parent, child);
}
usize PosixService::open_file_count() const
{
    return resources().files.open_count();
}
PosixService::FileDescriptor *PosixService::descriptor(Fd fd)
{
    const auto *slot = resources().files.entry(fd);
    return scheduler_ == nullptr || slot == nullptr || !slot->used ? nullptr
                                                                   : scheduler_->open_description(slot->description);
}
const PosixService::FileDescriptor *PosixService::descriptor(Fd fd) const
{
    const auto *slot = resources().files.entry(fd);
    return scheduler_ == nullptr || slot == nullptr || !slot->used ? nullptr
                                                                   : scheduler_->open_description(slot->description);
}
Result<Fd> PosixService::allocate_fd(Fd minimum)
{
    if (minimum < 0)
        return Status::invalid_argument("negative minimum descriptor");
    for (usize i = static_cast<usize>(minimum); i < max_open_files; ++i)
        if (!resources().files.contains(static_cast<Fd>(i)))
            return static_cast<Fd>(i);
    return Status::overflow("file descriptor table full");
}
Result<std::string_view> PosixService::normalize_path(std::string_view path)
{
    if (path.empty())
    {
        return Status::not_found("empty pathname");
    }
    if (path[0] == '/')
    {
        return path;
    }
    auto cwd = current_directory();
    if (!cwd)
        return cwd.status();
    normalized_path_.clear();
    if (auto status = normalized_path_.append(cwd.value()); !status.ok())
    {
        return status;
    }
    if (normalized_path_.view() != "/" && normalized_path_.view().back() != '/')
    {
        if (auto status = normalized_path_.append('/'); !status.ok())
        {
            return status;
        }
    }
    if (auto status = normalized_path_.append(path); !status.ok())
    {
        return status;
    }
    return normalized_path_.view();
}

Result<std::string_view> PosixService::resolve_path(Fd dirfd, std::string_view path)
{
    if (path.empty())
    {
        return Status::not_found("empty pathname");
    }
    if (path[0] == '/' || dirfd == at_FDCWD)
    {
        return normalize_path(path);
    }
    auto *entry = descriptor(dirfd);
    if (entry == nullptr || !entry->used || entry->node == nullptr)
        return Status::bad_descriptor("bad directory file descriptor");
    if (entry->node->metadata().type != fs::NodeType::directory)
        return Status::not_directory("file descriptor is not a directory");
    auto directory = vfs_->directory_path(*entry->node, entry->mount_id);
    if (!directory)
        return directory.status();
    normalized_path_.clear();
    if (auto status = normalized_path_.append(directory.value().view()); !status.ok())
    {
        return status;
    }
    if (normalized_path_.view() != "/" && normalized_path_.view().back() != '/')
    {
        if (auto status = normalized_path_.append('/'); !status.ok())
        {
            return status;
        }
    }
    if (auto status = normalized_path_.append(path); !status.ok())
    {
        return status;
    }
    return normalized_path_.view();
}

Status PosixService::copy_descriptor(Fd source, Fd target, u32 flags)
{
    auto *entry = resources().files.entry(source);
    if (scheduler_ == nullptr || entry == nullptr || !entry->used)
        return Status::invalid_argument("bad file descriptor");
    return scheduler_->install_descriptor(resources(), target, entry->description, (flags & o_CLOEXEC) != 0);
}
} // namespace ok::posix
